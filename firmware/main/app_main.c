#include "app_controller.h"

#include "board_audio.h"
#include "board_ui.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "livekit.h"
#include "livekit_terminal.h"
#include "media_pipeline.h"
#include "network.h"
#include "nvs_flash.h"
#include "speech_recognition.h"

static const char *TAG = "sparkbot_main";

static void reduce_rtc_log_noise(void)
{
    // esp_peer currently logs expected failed probes for every unusable ICE
    // candidate (Docker bridges and STUN reflexive addresses) at error level.
    // Connection state and genuine failures are already reported through the
    // LiveKit callbacks below, so suppress only these low-level trace tags.
    esp_log_level_set("AGENT", ESP_LOG_NONE);
    esp_log_level_set("PEER_DEF", ESP_LOG_NONE);
    esp_log_level_set("SCTP", ESP_LOG_NONE);
    esp_log_level_set("DTLS", ESP_LOG_NONE);
}

static void on_button(void)
{
    app_controller_post(APP_EVENT_BUTTON_TOGGLE);
}

static void on_button_long_press(void)
{
    app_controller_post(APP_EVENT_BUTTON_LONG_PRESS);
}

static void on_wake_word(void)
{
    app_controller_post(APP_EVENT_WAKE_WORD);
}

static void on_sleep_word(void)
{
    app_controller_post(APP_EVENT_SLEEP_WORD);
}

static void on_remote_audio_state_changed(bool speaking)
{
    app_controller_post(
        speaking ? APP_EVENT_REMOTE_AUDIO_STARTED : APP_EVENT_REMOTE_AUDIO_ENDED);
}

static void on_livekit_state(livekit_connection_state_t state)
{
    switch (state) {
        case LIVEKIT_CONNECTION_STATE_CONNECTED:
            app_controller_post(APP_EVENT_LIVEKIT_CONNECTED);
            break;
        case LIVEKIT_CONNECTION_STATE_DISCONNECTED:
            app_controller_post(APP_EVENT_LIVEKIT_DISCONNECTED);
            break;
        case LIVEKIT_CONNECTION_STATE_FAILED:
            app_controller_post(APP_EVENT_LIVEKIT_FAILED);
            break;
        default:
            break;
    }
}

static void on_sleep_drain_complete(void)
{
    app_controller_post(APP_EVENT_SLEEP_DRAIN_COMPLETE);
}

static void init_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    // The precompiled WiFi stack emits a continuous benign
    // "W wifi:m f null" for malformed management frames received from nearby
    // radios.  It is noise, not a link fault; keep only errors visible.
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    reduce_rtc_log_noise();
    ESP_LOGI(TAG, "starting SparkBot Jarvis terminal");

    init_nvs();
    ESP_ERROR_CHECK(livekit_system_init() == LIVEKIT_ERR_NONE ? ESP_OK : ESP_FAIL);
    // Initialize Wi-Fi before the audio/UI stacks so their internal-RAM
    // allocations (LVGL task, ESP-SR, codecs) cannot fragment the heap and
    // starve the Wi-Fi DMA buffer pool.
    ESP_ERROR_CHECK(network_init());
    ESP_ERROR_CHECK(board_audio_init());
    // Open the playback endpoint before any standby capture starts, so the
    // shared I2S TX slot is already stereo 16 kHz when av_render opens it.
    ESP_ERROR_CHECK(board_audio_preopen_playback());
#if CONFIG_SPARKBOT_ENABLE_FEEDBACK_TONES
    // Boot self-test: play the ready tone through the just-opened playback
    // endpoint so a silent speaker can be attributed to the DAC/PA chain
    // versus the remote-audio path.
    board_audio_play_tone(BOARD_TONE_READY);
#endif
    ESP_ERROR_CHECK(board_ui_init(on_button));
    board_ui_set_long_press_callback(on_button_long_press);
    ESP_ERROR_CHECK(media_pipeline_init());
    ESP_LOGI(
        TAG,
        "heap: internal=%u DMA=%u PSRAM=%u largest_internal=%u",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_DMA),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    media_pipeline_set_remote_audio_callback(on_remote_audio_state_changed);
    ESP_ERROR_CHECK(speech_recognition_start(
        media_pipeline_speech_sink(),
        on_wake_word,
        on_sleep_word));

    const esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2,
        ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org"));
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));

    ESP_ERROR_CHECK(livekit_terminal_init(on_livekit_state));
    livekit_terminal_set_sleep_ready_callback(on_sleep_drain_complete);
    ESP_ERROR_CHECK(app_controller_start());
    ESP_LOGI(TAG, "initialization complete; say Jarvis or press BOOT");
}
