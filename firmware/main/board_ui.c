#include "board_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// rgb565(238, 248, 255) and rgb565(114, 135, 160) as compile-time constants.
#define UI_FOREGROUND_RGB565 0xEFDF
#define UI_BACKGROUND_RGB565 0x7434

#define LCD_WIDTH        240
#define LCD_HEIGHT       240
#define ICON_WIDTH       120
#define ICON_HEIGHT      40
#define ICON_X           ((LCD_WIDTH - ICON_WIDTH) / 2)
#define ICON_Y           ((LCD_HEIGHT - ICON_HEIGHT) / 2)
#define LVGL_BUF_H       20 // 240x20x2B = 9.6 KiB, internal DMA: SPI sends
                            // the frame directly with no private copy buffer
#define ANIM_INTERVAL_MS 33 // ~30 fps for the icon region

#define LCD_MOSI      GPIO_NUM_47
#define LCD_CLOCK     GPIO_NUM_21
#define LCD_CHIP_SEL  GPIO_NUM_44
#define LCD_DATA_CMD  GPIO_NUM_43
#define LCD_BACKLIGHT GPIO_NUM_46
#define BOOT_BUTTON   GPIO_NUM_0

static const char *TAG = "sparkbot_ui";
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_lock;
static board_button_callback_t s_button_callback;
static board_button_callback_t s_button_long_press_callback;
static board_ui_state_t s_state = BOARD_UI_BOOTING;

// LVGL objects: one full-screen background plus a canvas that carries the
// per-state icon animation.  All animation drawing stays inside the 120x40
// canvas, so LVGL only invalidates/refreshes that small region.
static lv_display_t *s_disp;
static lv_obj_t *s_screen;
static lv_obj_t *s_canvas;
static lv_color_t *s_canvas_buf;
static lv_timer_t *s_anim_timer;
static uint32_t s_anim_phase;
static bool s_level_overlay;
static uint8_t s_level_bars;
static uint8_t s_level_count;
static TickType_t s_level_until;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} ui_rgb_t;

static const ui_rgb_t s_state_colors[BOARD_UI_ERROR + 1] = {
    [BOARD_UI_BOOTING]        = {32, 54, 92},
    [BOARD_UI_STANDBY]        = {8, 12, 22},
    [BOARD_UI_CONNECTING]     = {198, 120, 10},
    [BOARD_UI_LISTENING]      = {8, 135, 80},
    [BOARD_UI_PROCESSING]     = {86, 52, 142},
    [BOARD_UI_SLEEP_DRAINING] = {42, 37, 82},
    [BOARD_UI_SPEAKING]       = {42, 90, 210},
    [BOARD_UI_READY]          = {10, 46, 40},
    [BOARD_UI_ERROR]          = {190, 18, 30},
};

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

static void icon_set_pixel(
    uint16_t *pixels, int x, int y, uint16_t color)
{
    if (x >= 0 && x < ICON_WIDTH && y >= 0 && y < ICON_HEIGHT) {
        pixels[y * ICON_WIDTH + x] = color;
    }
}

static void icon_draw_point(
    uint16_t *pixels, int x, int y, int radius, uint16_t color)
{
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) {
                icon_set_pixel(pixels, x + dx, y + dy, color);
            }
        }
    }
}

static void icon_draw_line(
    uint16_t *pixels, int x0, int y0, int x1, int y1, int thickness,
    uint16_t color)
{
    const int dx = abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        icon_draw_point(pixels, x0, y0, thickness / 2, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void icon_draw_crescent(
    uint16_t *pixels, int center_x, int center_y, int radius,
    uint16_t foreground, uint16_t background)
{
    const int cutout_x = center_x + radius / 2;
    const int cutout_y = center_y - radius / 3;
    const int cutout_radius = radius - 2;
    for (int y = center_y - radius; y <= center_y + radius; ++y) {
        for (int x = center_x - radius; x <= center_x + radius; ++x) {
            const int outer_dx = x - center_x;
            const int outer_dy = y - center_y;
            if (outer_dx * outer_dx + outer_dy * outer_dy > radius * radius) {
                continue;
            }
            const int inner_dx = x - cutout_x;
            const int inner_dy = y - cutout_y;
            icon_set_pixel(
                pixels,
                x,
                y,
                inner_dx * inner_dx + inner_dy * inner_dy <= cutout_radius * cutout_radius
                    ? background
                    : foreground);
        }
    }
}

static void icon_fill(uint16_t *pixels, uint16_t color)
{
    for (size_t i = 0; i < ICON_WIDTH * ICON_HEIGHT; ++i) {
        pixels[i] = color;
    }
}

static uint16_t state_background_color(board_ui_state_t state)
{
    const ui_rgb_t *rgb = &s_state_colors[state];
    return rgb565(rgb->red, rgb->green, rgb->blue);
}

static void ui_draw_level_bars(uint16_t *pixels)
{
    const int gap = 2;
    const int margin = 2;
    const int bar_width =
        (ICON_WIDTH - (s_level_count - 1) * gap - margin * 2) / s_level_count;
    if (bar_width < 3) {
        return;
    }
    const int total_width =
        s_level_count * bar_width + (s_level_count - 1) * gap;
    const int start_x = (ICON_WIDTH - total_width) / 2;
    const uint16_t active = UI_FOREGROUND_RGB565;
    const uint16_t inactive = UI_BACKGROUND_RGB565;
    for (int bar = 0; bar < s_level_count; ++bar) {
        const int height = 5 + ((bar + 1) * 2);
        const int x0 = start_x + bar * (bar_width + gap);
        const int y0 = ICON_HEIGHT - 2 - height;
        const uint16_t color = bar < s_level_bars ? active : inactive;
        for (int y = y0; y < y0 + height; ++y) {
            for (int x = x0; x < x0 + bar_width; ++x) {
                icon_set_pixel(pixels, x, y, color);
            }
        }
    }
}

static void ui_draw_state_frame(uint16_t *pixels)
{
    const uint16_t background = state_background_color(s_state);
    const uint16_t foreground = UI_FOREGROUND_RGB565;
    icon_fill(pixels, background);

    if (s_level_overlay) {
        ui_draw_level_bars(pixels);
        return;
    }

    switch (s_state) {
        case BOARD_UI_STANDBY: {
            // Breathing ring: radius slowly oscillates around the center.
            const double t = (double)(s_anim_phase % 2000) / 2000.0;
            const int radius = 13 + (int)(4.0 * sin(2.0 * M_PI * t));
            for (int a = 0; a < 360; a += 6) {
                const double rad = a * M_PI / 180.0;
                icon_draw_point(
                    pixels,
                    ICON_WIDTH / 2 + (int)(radius * cos(rad)),
                    ICON_HEIGHT / 2 + (int)(radius * sin(rad)),
                    1,
                    foreground);
            }
            icon_draw_point(pixels, ICON_WIDTH / 2, ICON_HEIGHT / 2, 2, foreground);
            break;
        }
        case BOARD_UI_CONNECTING:
        case BOARD_UI_PROCESSING: {
            // Rotating arc: three dots orbit the center.
            const int radius = 15;
            for (int dot = 0; dot < 3; ++dot) {
                const double angle =
                    (double)((s_anim_phase / 2) % 360) * M_PI / 180.0 +
                    dot * 2.0 * M_PI / 3.0;
                icon_draw_point(
                    pixels,
                    ICON_WIDTH / 2 + (int)(radius * cos(angle)),
                    ICON_HEIGHT / 2 + (int)(radius * sin(angle)),
                    dot == 0 ? 3 : 2,
                    foreground);
            }
            break;
        }
        case BOARD_UI_LISTENING: {
            // Waveform: five bars whose heights follow a smooth wave.
            for (int bar = 0; bar < 5; ++bar) {
                const double phase =
                    (double)(s_anim_phase / 3) * 0.35 + bar * 0.9;
                const int height =
                    5 + (int)((sin(phase) + 1.0) * 0.5 * 18.0);
                const int x0 = ICON_WIDTH / 2 - 30 + bar * 15;
                for (int y = ICON_HEIGHT / 2 + 10 - height;
                     y < ICON_HEIGHT / 2 + 10;
                     ++y) {
                    icon_set_pixel(pixels, x0, y, foreground);
                    icon_set_pixel(pixels, x0 + 3, y, foreground);
                }
            }
            break;
        }
        case BOARD_UI_SPEAKING: {
            // Equalizer pulse while the agent speaks.
            for (int bar = 0; bar < 5; ++bar) {
                const double phase =
                    (double)(s_anim_phase / 2) * 0.5 + bar * 1.1;
                const int height =
                    6 + (int)((sin(phase) * 0.5 + 0.5) * 22.0);
                const int x0 = ICON_WIDTH / 2 - 30 + bar * 15;
                for (int y = ICON_HEIGHT / 2 + 10 - height;
                     y < ICON_HEIGHT / 2 + 10;
                     ++y) {
                    icon_set_pixel(pixels, x0, y, foreground);
                    icon_set_pixel(pixels, x0 + 3, y, foreground);
                }
            }
            break;
        }
        case BOARD_UI_READY: {
            // Bold checkmark: initialization/connection complete, the user
            // may say the wake word now.  Slight pulse keeps it alive.
            const int pulse = ((s_anim_phase / 400) % 2) ? 5 : 6;
            const uint16_t check = rgb565(0, 210, 110);
            icon_draw_line(pixels, 28, 22, 46, 32, pulse, check);
            icon_draw_line(pixels, 46, 32, 74, 10, pulse, check);
            break;
        }
        case BOARD_UI_SLEEP_DRAINING: {
            // Moon with a slow glow: outer dim ring + crescent.
            const double t = (double)(s_anim_phase % 2000) / 2000.0;
            const int glow = 17 + (int)(2.0 * sin(2.0 * M_PI * t));
            for (int a = 0; a < 360; a += 6) {
                const double rad = a * M_PI / 180.0;
                icon_draw_point(
                    pixels,
                    50 + (int)(glow * cos(rad)),
                    20 + (int)(glow * sin(rad)),
                    1,
                    background == rgb565(42, 37, 82) ? rgb565(72, 67, 112)
                                                     : foreground);
            }
            icon_draw_crescent(pixels, 50, 20, 15, foreground, background);
            icon_draw_point(pixels, 76, 20, 3, foreground);
            icon_draw_point(pixels, 88, 20, 4, foreground);
            icon_draw_point(pixels, 100, 20, 3, foreground);
            break;
        }
        case BOARD_UI_ERROR:
        default: {
            // Blinking cross.
            if ((s_anim_phase / 500) % 2 == 0) {
                icon_draw_line(pixels, 42, 8, 78, 32, 5, foreground);
                icon_draw_line(pixels, 78, 8, 42, 32, 5, foreground);
            }
            break;
        }
    }
}

static void ui_anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_anim_phase += ANIM_INTERVAL_MS;
    if (s_level_overlay &&
        xTaskGetTickCount() >= s_level_until) {
        s_level_overlay = false;
    }
    lvgl_port_lock(0);
    uint16_t *pixels = (uint16_t *)s_canvas_buf;
    ui_draw_state_frame(pixels);
    lv_obj_invalidate(s_canvas);
    lvgl_port_unlock();
}

static bool ui_flush_ready_cb(
    esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_io_event_data_t *data,
    void *user_ctx)
{
    (void)io;
    (void)data;
    lv_disp_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void ui_flush_cb(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
    // ST7789 expects big-endian RGB565 while LVGL buffers are little-endian.
    uint8_t *pixel = color_map;
    const uint32_t pixel_count =
        (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
    for (uint32_t i = 0; i < pixel_count; ++i) {
        uint8_t tmp = pixel[0];
        pixel[0] = pixel[1];
        pixel[1] = tmp;
        pixel += 2;
    }
    esp_err_t result = esp_lcd_panel_draw_bitmap(
        s_panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        color_map);
    if (result != ESP_OK) {
        // Never let a failed transfer stall the LVGL refresh loop: signal
        // ready immediately and skip the frame.  esp_lvgl_port's own flush
        // callback ignores the error, so LVGL then waits forever for a
        // completion callback that never arrives (task WDT abort).
        ESP_LOGW(TAG, "LCD flush failed (%s); skipping frame", esp_err_to_name(result));
        lv_disp_flush_ready(drv);
    }
}

static void button_task(void *unused)
{
    (void)unused;
    bool was_pressed = false;
    TickType_t pressed_since = 0;

    while (true) {
        const bool pressed = gpio_get_level(BOOT_BUTTON) == 0;
        if (pressed && !was_pressed) {
            pressed_since = xTaskGetTickCount();
        } else if (!pressed && was_pressed) {
            const TickType_t held = xTaskGetTickCount() - pressed_since;
            if (held >= pdMS_TO_TICKS(BOARD_BUTTON_LONG_PRESS_MS)) {
                if (s_button_long_press_callback) {
                    s_button_long_press_callback();
                }
            } else if (held >= pdMS_TO_TICKS(40) && s_button_callback) {
                s_button_callback();
            }
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void board_ui_set_long_press_callback(board_button_callback_t callback)
{
    s_button_long_press_callback = callback;
}

esp_err_t board_ui_init(board_button_callback_t button_callback)
{
    s_button_callback = button_callback;

    const gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "configure UI outputs");
    gpio_set_level(LCD_BACKLIGHT, 0);

    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "configure BOOT button");

    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_CLOCK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LVGL_BUF_H * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG,
        "initialize LCD SPI bus");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DATA_CMD,
        .cs_gpio_num = LCD_CHIP_SEL,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 2,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &s_panel_io),
        TAG,
        "create LCD panel IO");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel),
        TAG,
        "create ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "initialize LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert LCD colors");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "enable LCD");

    s_lcd_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lcd_lock, ESP_ERR_NO_MEM, TAG, "create LCD mutex");

    // LVGL init: task/timer + display attached to the existing panel.
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // Keep the 7 KiB LVGL task stack out of internal RAM: the WiFi DMA pool
    // needs the contiguous internal heap to initialize reliably.
    lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "initialize LVGL port");

    // One static canvas carries every state animation.
    s_canvas_buf = heap_caps_malloc(
        ICON_WIDTH * ICON_HEIGHT * sizeof(lv_color_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_canvas_buf) {
        s_canvas_buf = heap_caps_malloc(
            ICON_WIDTH * ICON_HEIGHT * sizeof(lv_color_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_canvas_buf, ESP_ERR_NO_MEM, TAG, "allocate icon canvas");

    lvgl_port_lock(0);
    // Attach the display directly instead of esp_lvgl_port_add_disp():
    // 1) a DMA-capable internal draw buffer lets the SPI master send frames
    //    without a lazily-allocated private DMA copy, which could fail under
    //    memory pressure; and
    // 2) our flush callback always signals LVGL when a transfer fails.
    s_disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (!s_disp) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    void *draw_buf = heap_caps_aligned_alloc(
        CONFIG_LV_DRAW_BUF_ALIGN,
        LCD_WIDTH * LVGL_BUF_H * sizeof(uint16_t),
        MALLOC_CAP_DMA);
    if (!draw_buf) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(
        s_disp,
        draw_buf,
        NULL,
        LCD_WIDTH * LVGL_BUF_H * sizeof(uint16_t),
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, ui_flush_cb);
    const esp_lcd_panel_io_callbacks_t panel_io_cbs = {
        .on_color_trans_done = ui_flush_ready_cb,
    };
    esp_err_t io_cb_result =
        esp_lcd_panel_io_register_event_callbacks(s_panel_io, &panel_io_cbs, s_disp);
    if (io_cb_result != ESP_OK) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "register LCD flush completion callback failed: %s", esp_err_to_name(io_cb_result));
        return ESP_ERR_INVALID_STATE;
    }
    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(
        s_screen,
        lv_color_make(
            s_state_colors[BOARD_UI_BOOTING].red,
            s_state_colors[BOARD_UI_BOOTING].green,
            s_state_colors[BOARD_UI_BOOTING].blue),
        0);
    s_canvas = lv_canvas_create(s_screen);
    lv_canvas_set_buffer(
        s_canvas,
        s_canvas_buf,
        ICON_WIDTH,
        ICON_HEIGHT,
        LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, ICON_X, ICON_Y);
    s_anim_timer = lv_timer_create(ui_anim_timer_cb, ANIM_INTERVAL_MS, NULL);
    lvgl_port_unlock();

    gpio_set_level(LCD_BACKLIGHT, 1);

    BaseType_t task_result = xTaskCreate(
        button_task,
        "sparkbot_button",
        2048,
        NULL,
        3,
        NULL);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_FAIL, TAG, "start button task");

    board_ui_set_state(BOARD_UI_BOOTING);
    return ESP_OK;
}

void board_ui_set_state(board_ui_state_t state)
{
    if (state > BOARD_UI_ERROR) {
        state = BOARD_UI_ERROR;
    }
    s_state = state;
    s_level_overlay = false;
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(
        s_screen,
        lv_color_make(
            s_state_colors[state].red,
            s_state_colors[state].green,
            s_state_colors[state].blue),
        0);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "state=%d", state);
}

void board_ui_show_volume(int level, int level_count)
{
    s_level_overlay = true;
    s_level_bars = (uint8_t)level;
    s_level_count = (uint8_t)level_count;
    s_level_until = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
    ESP_LOGI(TAG, "volume display: %d/%d", level, level_count);
}

void board_ui_show_sensitivity(int level, int level_count)
{
    s_level_overlay = true;
    s_level_bars = (uint8_t)level;
    s_level_count = (uint8_t)level_count;
    s_level_until = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
    ESP_LOGI(TAG, "sensitivity display: %d/%d", level, level_count);
}

void board_ui_pulse(void)
{
    board_ui_set_state(s_state);
}
