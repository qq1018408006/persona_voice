#include "board_audio.h"

#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOARD_I2C_SDA  GPIO_NUM_4
#define BOARD_I2C_SCL  GPIO_NUM_5
#define BOARD_I2S_MCLK GPIO_NUM_45
#define BOARD_I2S_BCLK GPIO_NUM_39
#define BOARD_I2S_WS   GPIO_NUM_41
#define BOARD_I2S_DOUT GPIO_NUM_42
#define BOARD_I2S_DIN  GPIO_NUM_40
// SparkBot hardware (oshwhub esp-college/esp-sparkbot, ADF board def
// esp32_s3_sparkbot) routes the speaker power-amplifier enable through
// GPIO46, the same net used for the LCD backlight.  The BSP drives it as
// backlight; the ADF audio board defines it as PA_ENABLE_GPIO.  We drive it
// explicitly here and keep it high, so the boot self-test tone and every
// remote-audio render are audible even before/without the UI init.
#define BOARD_SPEAKER_EN_GPIO GPIO_NUM_46
#define BOARD_PA_PIN   GPIO_NUM_NC

#define AUDIO_RATE       16000
#define TONE_FRAME_MS    20
#define TONE_FRAME_SAMPLES ((AUDIO_RATE * TONE_FRAME_MS) / 1000)

static const char *TAG = "sparkbot_audio";

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static esp_codec_dev_handle_t s_play_dev;
static esp_codec_dev_handle_t s_record_dev;
static SemaphoreHandle_t s_tone_lock;
static int s_mic_gain_db = CONFIG_SPARKBOT_MIC_GAIN_DB;

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &s_i2c_bus);
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    // LCD is now a small persistent debug buffer, so I2S can use the original
    // descriptor count and slightly larger frame buffers for stable startup.
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 288;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_config, &s_i2s_tx, &s_i2s_rx),
        TAG,
        "create duplex I2S channel");

    i2s_std_config_t stream_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_tx, &stream_config),
        TAG,
        "initialize I2S TX");
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_i2s_rx, &stream_config),
        TAG,
        "initialize I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "enable I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "enable I2S RX");
    return ESP_OK;
}

static esp_err_t init_codecs(void)
{
    audio_codec_i2s_cfg_t i2s_config = {
        .port = I2S_NUM_0,
        .tx_handle = s_i2s_tx,
        .rx_handle = s_i2s_rx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_config);
    ESP_RETURN_ON_FALSE(data_if, ESP_ERR_NO_MEM, TAG, "create codec I2S data interface");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_ERR_NO_MEM, TAG, "create codec GPIO interface");

    audio_codec_i2c_cfg_t control_config = {
        .port = I2C_NUM_0,
        .bus_handle = s_i2c_bus,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
    };
    const audio_codec_ctrl_if_t *control_if = audio_codec_new_i2c_ctrl(&control_config);
    ESP_RETURN_ON_FALSE(control_if, ESP_ERR_NO_MEM, TAG, "create ES8311 control interface");

    es8311_codec_cfg_t codec_config = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        // Deliberately NC: with a real pa_pin the codec layer would pull it
        // low in es8311_close() whenever av_render pauses/standbys, which
        // would also kill the shared LCD backlight net.  The PA/backlight
        // pin is managed manually in board_audio_preopen_playback() instead.
        .pa_pin = BOARD_PA_PIN,
        .use_mclk = true,
        .digital_mic = false,
        .hw_gain = { .pa_gain = 0.0f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "create duplex ES8311 codec");

    esp_codec_dev_cfg_t playback_device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_play_dev = esp_codec_dev_new(&playback_device_config);
    ESP_RETURN_ON_FALSE(s_play_dev, ESP_FAIL, TAG, "create playback device");

    esp_codec_dev_cfg_t record_device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_record_dev = esp_codec_dev_new(&record_device_config);
    ESP_RETURN_ON_FALSE(s_record_dev, ESP_FAIL, TAG, "create record device");

    // The two logical endpoints share one physical duplex codec. Closing one
    // endpoint must not suspend the ES8311 while the other is still in use.
    ESP_RETURN_ON_ERROR(
        esp_codec_set_disable_when_closed(s_play_dev, false),
        TAG,
        "keep duplex codec active after playback close");
    ESP_RETURN_ON_ERROR(
        esp_codec_set_disable_when_closed(s_record_dev, false),
        TAG,
        "keep duplex codec active after record close");

    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_out_vol(s_play_dev, CONFIG_SPARKBOT_SPEAKER_VOLUME),
        TAG,
        "set speaker volume");
    ESP_RETURN_ON_ERROR(
        esp_codec_dev_set_in_gain(s_record_dev, (float)s_mic_gain_db),
        TAG,
        "set microphone gain");
    return ESP_OK;
}

esp_err_t board_audio_init(void)
{
    ESP_LOGI(TAG, "initializing SparkBot ES8311 audio");
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "initialize I2C");
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "initialize I2S");
    ESP_RETURN_ON_ERROR(init_codecs(), TAG, "initialize ES8311");
    s_tone_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tone_lock, ESP_ERR_NO_MEM, TAG, "create tone mutex");
    return ESP_OK;
}

esp_err_t board_audio_preopen_playback(void)
{
    if (!s_play_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    // Power the speaker PA (and the shared LCD backlight net) before any
    // tone/remote audio.  The UI also drives this pin at display init, but
    // the boot self-test tone plays before that, so do it here as well.
    // GPIO_MODE_INPUT_OUTPUT (not plain OUTPUT) so the state log can read
    // the real pad level back; a pure-output pad has its input buffer
    // disabled and gpio_get_level() would always report 0.
    const gpio_config_t pa_enable = {
        .pin_bit_mask = 1ULL << BOARD_SPEAKER_EN_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_config(&pa_enable),
        TAG,
        "configure speaker PA enable");
    gpio_set_level(BOARD_SPEAKER_EN_GPIO, 1);

    // The standby microphone path opens the shared I2S data interface first
    // and, while it is running, the codec layer deliberately blocks any
    // attempt to disable/reconfigure the I2S TX channel ("Pending out
    // channel for in channel running").  Opening the playback endpoint here
    // with the exact format av_render will later request makes the renderer's
    // first open a no-op, so the TX slot stays stereo 16 kHz and Gemini audio
    // can never hit the 0x103 reconfig-while-running failure at wake.
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    const int result = esp_codec_dev_open(s_play_dev, &format);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "pre-open playback device failed: %d", result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "playback endpoint pre-opened: 16 kHz stereo");
    board_audio_log_audio_state("after preopen");
    return ESP_OK;
}

void board_audio_log_audio_state(const char *why)
{
    if (s_i2s_tx) {
        i2s_chan_info_t info = {0};
        if (i2s_channel_get_info(s_i2s_tx, &info) == ESP_OK) {
            ESP_LOGI(
                TAG,
                "%s: I2S TX enabled=%d mclk=%lu bclk=%lu",
                why,
                info.is_enabled,
                (unsigned long)info.mclk_hz,
                (unsigned long)info.bclk_hz);
        } else {
            ESP_LOGI(TAG, "%s: I2S TX info unavailable", why);
        }
    }
    if (s_play_dev) {
        // 0x00 reset, 0x01 clk src, 0x02-0x08 codec dividers,
        // 0x09/0x0A SDP in/out, 0x0B/0x0C system, 0x0D power, 0x0E system,
        // 0x12 DAC enable, 0x14 PGA/DMIC, 0x31 mute, 0x32 volume, 0x37 ramp.
        const int regs[] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x12, 0x14, 0x31, 0x32, 0x37,
        };
        char line[sizeof("00:00 01:00 02:00 03:00 04:00 05:00 06:00 07:00 "
                          "08:00 09:00 0A:00 0B:00 0C:00 0D:00 0E:00 12:00 "
                          "14:00 31:00 32:00 37:00")];
        size_t pos = 0;
        for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
            int value = -1;
            const int ret = esp_codec_dev_read_reg(s_play_dev, regs[i], &value);
            pos += snprintf(
                line + pos,
                sizeof(line) - pos,
                "%02X:%02X%s",
                regs[i],
                ret == 0 ? (value & 0xFF) : 0xEE,
                i + 1 < sizeof(regs) / sizeof(regs[0]) ? " " : "");
        }
        ESP_LOGI(TAG, "%s: ES8311 %s", why, line);
    }
    if (BOARD_SPEAKER_EN_GPIO != GPIO_NUM_NC) {
        ESP_LOGI(
            TAG,
            "%s: speaker PA/backlight GPIO%d level=%d",
            why,
            BOARD_SPEAKER_EN_GPIO,
            gpio_get_level(BOARD_SPEAKER_EN_GPIO));
    }
}

esp_codec_dev_handle_t board_audio_record_handle(void)
{
    return s_record_dev;
}

esp_codec_dev_handle_t board_audio_playback_handle(void)
{
    return s_play_dev;
}

esp_err_t board_audio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_codec_dev_set_out_vol(s_play_dev, volume);
}

esp_err_t board_audio_set_mic_gain_db(int gain_db)
{
    if (!s_record_dev || gain_db < 0 || gain_db > 48) {
        return ESP_ERR_INVALID_ARG;
    }
    int result = esp_codec_dev_set_in_gain(s_record_dev, (float)gain_db);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set microphone gain to %d dB failed: %d", gain_db, result);
        return ESP_FAIL;
    }
    s_mic_gain_db = gain_db;
    ESP_LOGI(TAG, "microphone input gain set to %d dB", s_mic_gain_db);
    return ESP_OK;
}

int board_audio_get_mic_gain_db(void)
{
    return s_mic_gain_db;
}

static esp_err_t write_square_tone(int frequency_hz, int duration_ms)
{
    // The SparkBot codec uses a two-slot I2S frame.  Duplicate the short
    // local tone into both slots so its route matches remote speech exactly.
    int16_t samples[TONE_FRAME_SAMPLES * 2];
    const int half_period = AUDIO_RATE / (frequency_hz * 2);
    int phase = 0;
    int polarity = 1;
    const int frame_count = (duration_ms + TONE_FRAME_MS - 1) / TONE_FRAME_MS;

    for (int frame = 0; frame < frame_count; ++frame) {
        for (int i = 0; i < TONE_FRAME_SAMPLES; ++i) {
            // ~55% full scale: clearly audible through the 3 W PA even at
            // 70 % DAC volume, so the boot tone is a reliable speaker test.
            const int16_t sample = (int16_t)(polarity * 9000);
            samples[2 * i] = sample;
            samples[2 * i + 1] = sample;
            if (++phase >= half_period) {
                phase = 0;
                polarity = -polarity;
            }
        }
        ESP_RETURN_ON_ERROR(
            esp_codec_dev_write(s_play_dev, samples, sizeof(samples)),
            TAG,
            "write feedback tone");
    }
    return ESP_OK;
}

esp_err_t board_audio_play_tone(board_tone_t tone)
{
#if !CONFIG_SPARKBOT_ENABLE_FEEDBACK_TONES
    (void)tone;
    return ESP_OK;
#else
    if (!s_play_dev || !s_tone_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_tone_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_err_t result = esp_codec_dev_open(s_play_dev, &format);
    if (result == ESP_OK) {
        switch (tone) {
            case BOARD_TONE_WAKE_DETECTED:
                result = write_square_tone(880, 100);
                break;
            case BOARD_TONE_READY:
                result = write_square_tone(1040, 150);
                if (result == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(60));
                    result = write_square_tone(1320, 200);
                }
                break;
            case BOARD_TONE_STANDBY:
                result = write_square_tone(520, 180);
                break;
            case BOARD_TONE_ERROR:
                result = write_square_tone(260, 260);
                break;
        }
        // Deliberately keep the playback endpoint open.  Closing it clears
        // output_opened/out_enable even though disable_when_closed=false, and
        // the next av_render open would then try to reconfigure the I2S TX
        // slot while the standby capture keeps it running -> 0x103 -> the
        // first Gemini reply is silently dropped (the old recurring bug).
        // The DAC stays enabled anyway, so leaving the endpoint open is the
        // same hardware state with a healthier software state.
    }
    xSemaphoreGive(s_tone_lock);
    return result;
#endif
}
