#include "speech_recognition.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

#define AFE_FEED_TOKEN_LIMIT 32
#define AUDIO_DIAGNOSTIC_INTERVAL_MS 2000
// After a wake word, ignore "Jarvis" in the active state for this long so the
// tail of the wake utterance cannot immediately put the device back to sleep.
#define ACTIVE_SLEEP_GUARD_MS 2000

typedef enum {
    SR_MODE_PAUSED = 0,
    SR_MODE_STANDBY,
    SR_MODE_ACTIVE,
} local_sr_mode_t;

static const char *TAG = "sparkbot_sr";
static esp_capture_sink_handle_t s_pcm_sink;
static speech_event_callback_t s_on_wake;
static speech_event_callback_t s_on_sleep;
static volatile local_sr_mode_t s_mode = SR_MODE_PAUSED;

static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static SemaphoreHandle_t s_afe_feed_tokens;
// Serializes pcm_feed_task's sink access with speech_recognition_pause():
// the controller tears down the capture pipeline while handing the
// microphone between standby and LiveKit, and a sink call racing that
// teardown hit a destroyed data queue (spinlock assert in data_q_have_data).
static SemaphoreHandle_t s_pcm_lock;
static float s_wake_threshold;
static volatile vad_state_t s_last_vad_state = VAD_SILENCE;
static volatile TickType_t s_active_sleep_ignore_until;
// Incremented on every successful capture-frame acquire.  The controller
// watches this in standby so a dead LiveKit-owned capture (e.g. after the
// room drops while warm-stanbding) can be detected and restarted locally.
static volatile uint32_t s_capture_heartbeat;

static void clear_feed_tokens(void)
{
    if (!s_afe_feed_tokens) {
        return;
    }
    while (xSemaphoreTake(s_afe_feed_tokens, 0) == pdTRUE) {
    }
}

static void pcm_feed_task(void *unused)
{
    (void)unused;
    const int feed_samples = s_afe->get_feed_chunksize(s_afe_data);
    const int feed_channels = s_afe->get_feed_channel_num(s_afe_data);
    const size_t target_bytes =
        (size_t)feed_samples * (size_t)feed_channels * sizeof(int16_t);
    int16_t *feed_buffer = heap_caps_malloc(
        target_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!feed_buffer) {
        ESP_LOGE(TAG, "unable to allocate AFE feed buffer");
        vTaskDelete(NULL);
        return;
    }

    size_t buffered_bytes = 0;
    bool first_feed = true;
    TickType_t last_capture_error = 0;
#if CONFIG_SPARKBOT_SR_DIAGNOSTICS
    TickType_t last_level_log = xTaskGetTickCount();
    uint64_t level_sum = 0;
    uint32_t level_samples = 0;
    uint32_t level_peak = 0;
#endif
    while (true) {
        if (xSemaphoreTake(s_pcm_lock, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Capture is deliberately stopped while the controller hands the
        // microphone between standby recognition and LiveKit.  Do not touch
        // the sink queue while it is being torn down; esp_capture otherwise
        // repeatedly reports "q not created".
        if (s_mode == SR_MODE_PAUSED) {
            xSemaphoreGive(s_pcm_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
        };
        esp_capture_err_t capture_result =
            esp_capture_sink_acquire_frame(s_pcm_sink, &frame, false);
        if (capture_result != ESP_CAPTURE_ERR_OK) {
            TickType_t now = xTaskGetTickCount();
            if (s_mode != SR_MODE_PAUSED &&
                (last_capture_error == 0 ||
                 now - last_capture_error >= pdMS_TO_TICKS(2000))) {
                ESP_LOGW(TAG, "PCM sink acquire failed: %d", capture_result);
                last_capture_error = now;
            }
            xSemaphoreGive(s_pcm_lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        ++s_capture_heartbeat;
        last_capture_error = 0;

        if (!frame.data || frame.size <= 0) {
            if (s_mode != SR_MODE_PAUSED) {
                ESP_LOGW(TAG, "PCM sink returned an empty frame");
            }
            esp_capture_sink_release_frame(s_pcm_sink, &frame);
            xSemaphoreGive(s_pcm_lock);
            continue;
        }

        const uint8_t *source = frame.data;
        size_t remaining = (size_t)frame.size;
        while (remaining > 0) {
            size_t copy_size = target_bytes - buffered_bytes;
            if (copy_size > remaining) {
                copy_size = remaining;
            }
            memcpy((uint8_t *)feed_buffer + buffered_bytes, source, copy_size);
            source += copy_size;
            remaining -= copy_size;
            buffered_bytes += copy_size;
            if (buffered_bytes == target_bytes) {
#if CONFIG_SPARKBOT_SR_DIAGNOSTICS
                for (size_t i = 0; i < target_bytes / sizeof(int16_t); ++i) {
                    int32_t sample = feed_buffer[i];
                    uint32_t magnitude =
                        (uint32_t)(sample < 0 ? -sample : sample);
                    level_sum += magnitude;
                    ++level_samples;
                    if (magnitude > level_peak) {
                        level_peak = magnitude;
                    }
                }
#endif

                int fed = s_afe->feed(s_afe_data, feed_buffer);
                if (fed < 0) {
                    ESP_LOGE(TAG, "AFE feed failed: %d", fed);
                } else {
                    // One completed AFE input block permits one fetch. A binary
                    // ready flag would allow the detect task to over-fetch the
                    // ring buffer after the first block.
                    if (s_mode != SR_MODE_PAUSED) {
                        xSemaphoreGive(s_afe_feed_tokens);
                    }
                    if (first_feed) {
                        first_feed = false;
                        ESP_LOGI(
                            TAG,
                            "first PCM block fed to AFE: %d samples x %d channel(s)",
                            feed_samples,
                            feed_channels);
                    }
                }
                buffered_bytes = 0;

#if CONFIG_SPARKBOT_SR_DIAGNOSTICS
                TickType_t now = xTaskGetTickCount();
                if (now - last_level_log >=
                    pdMS_TO_TICKS(AUDIO_DIAGNOSTIC_INTERVAL_MS)) {
                    uint32_t mean_abs =
                        level_samples ? (uint32_t)(level_sum / level_samples) : 0;
                    ESP_LOGI(
                        TAG,
                        "microphone PCM level: peak=%" PRIu32 " mean_abs=%" PRIu32,
                        level_peak,
                        mean_abs);
                    level_sum = 0;
                    level_samples = 0;
                    level_peak = 0;
                    last_level_log = now;
                }
#endif
            }
        }
        esp_capture_sink_release_frame(s_pcm_sink, &frame);
        xSemaphoreGive(s_pcm_lock);
    }
}

static void recognition_task(void *unused)
{
    (void)unused;
#if CONFIG_SPARKBOT_SR_DIAGNOSTICS
    TickType_t last_afe_log = xTaskGetTickCount();
    uint32_t fetch_count = 0;
#endif
    while (true) {
        if (s_mode == SR_MODE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (xSemaphoreTake(s_afe_feed_tokens, pdMS_TO_TICKS(250)) != pdTRUE ||
            s_mode == SR_MODE_PAUSED) {
            continue;
        }

        afe_fetch_result_t *result = s_afe->fetch(s_afe_data);
        if (!result || result->ret_value == ESP_FAIL) {
            continue;
        }

#if CONFIG_SPARKBOT_SR_DIAGNOSTICS
        ++fetch_count;

        if (result->vad_state != s_last_vad_state) {
            ESP_LOGI(
                TAG,
                "AFE VAD: %s (volume=%.1f dB)",
                result->vad_state == VAD_SPEECH ? "speech" : "silence",
                (double)result->data_volume);
        }
        TickType_t now = xTaskGetTickCount();
        if (now - last_afe_log >=
            pdMS_TO_TICKS(AUDIO_DIAGNOSTIC_INTERVAL_MS)) {
            ESP_LOGI(
                TAG,
                "AFE running: fetches=%" PRIu32 " volume=%.1f dB vad=%s wake_state=%d",
                fetch_count,
                (double)result->data_volume,
                result->vad_state == VAD_SPEECH ? "speech" : "silence",
                (int)result->wakeup_state);
            fetch_count = 0;
            last_afe_log = now;
        }
#endif

        if (result->wakeup_state == WAKENET_DETECTED ||
            result->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            if (s_mode == SR_MODE_STANDBY) {
                s_mode = SR_MODE_PAUSED;
                s_afe->disable_wakenet(s_afe_data);
                s_last_vad_state = VAD_SILENCE;
                ESP_LOGI(TAG, "local wake word detected");
                if (s_on_wake) {
                    s_on_wake();
                }
                continue;
            }
            if (s_mode == SR_MODE_ACTIVE) {
                // Same wake word doubles as the local standby trigger:
                // saying "Jarvis" during a conversation requests orderly
                // standby through the same path as the old local sleep
                // phrase.  The guard keeps the wake utterance tail from
                // immediately sleeping the device.
                const TickType_t now = xTaskGetTickCount();
                if ((int32_t)(now - s_active_sleep_ignore_until) < 0) {
                    continue;
                }
                s_active_sleep_ignore_until =
                    now + pdMS_TO_TICKS(ACTIVE_SLEEP_GUARD_MS);
                s_mode = SR_MODE_PAUSED;
                s_afe->disable_wakenet(s_afe_data);
                s_last_vad_state = VAD_SILENCE;
                ESP_LOGI(TAG, "local sleep word detected (Jarvis during active)");
                if (s_on_sleep) {
                    s_on_sleep();
                }
                continue;
            }
        }

        s_last_vad_state = result->vad_state;

        // WakeNet inference is CPU-intensive and, when capture has a small
        // backlog, this task can otherwise immediately fetch another block
        // without giving CPU0's idle task any run time.  One tick does not
        // affect the 32 ms AFE cadence but prevents a false task-WDT reset.
        vTaskDelay(1);
    }
}

esp_err_t speech_recognition_start(
    esp_capture_sink_handle_t pcm_sink,
    speech_event_callback_t on_wake,
    speech_event_callback_t on_sleep)
{
    ESP_RETURN_ON_FALSE(pcm_sink, ESP_ERR_INVALID_ARG, TAG, "PCM sink is required");
    s_pcm_sink = pcm_sink;
    s_on_wake = on_wake;
    s_on_sleep = on_sleep;

    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models, ESP_ERR_NOT_FOUND, TAG, "ESP-SR model partition missing");

    afe_config_t *config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "create AFE configuration");
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    // Give the recognition task more headroom during AEC-heavy playback so a
    // temporary scheduling delay does not immediately drop WakeNet frames.
    config->afe_ringbuf_size = 16;
    s_afe = esp_afe_handle_from_config(config);
    if (!s_afe) {
        afe_config_free(config);
        return ESP_FAIL;
    }
    s_afe_data = s_afe->create_from_config(config);
    afe_config_free(config);
    ESP_RETURN_ON_FALSE(s_afe_data, ESP_ERR_NO_MEM, TAG, "create AFE instance");
    s_afe_feed_tokens = xSemaphoreCreateCounting(AFE_FEED_TOKEN_LIMIT, 0);
    ESP_RETURN_ON_FALSE(
        s_afe_feed_tokens,
        ESP_ERR_NO_MEM,
        TAG,
        "create AFE feed semaphore");
    s_pcm_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(
        s_pcm_lock,
        ESP_ERR_NO_MEM,
        TAG,
        "create PCM feed lock");

    ESP_LOGI(
        TAG,
        "AFE frame sizes: feed=%d samples x %d channel(s), fetch=%d samples",
        s_afe->get_feed_chunksize(s_afe_data),
        s_afe->get_feed_channel_num(s_afe_data),
        s_afe->get_fetch_chunksize(s_afe_data));

    float threshold = strtof(CONFIG_SPARKBOT_WAKE_THRESHOLD, NULL);
    ESP_RETURN_ON_ERROR(
        speech_recognition_set_detection_threshold(threshold),
        TAG,
        "set initial WakeNet threshold");

    BaseType_t result = xTaskCreatePinnedToCore(
        pcm_feed_task,
        "sparkbot_sr_feed",
        4096,
        NULL,
        6,
        NULL,
        1);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_FAIL, TAG, "start AFE feed task");
    result = xTaskCreatePinnedToCore(
        recognition_task,
        "sparkbot_sr_detect",
        6144,
        NULL,
        6,
        NULL,
        0);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_FAIL, TAG, "start recognition task");

    // The controller starts microphone capture before it enters standby.
    // Stay paused here so fetch never races ahead of the first AFE feed.
    ESP_LOGI(TAG, "local Jarvis wake/sleep toggle recognition initialized");
    return ESP_OK;
}

void speech_recognition_enter_standby(void)
{
    if (!s_afe_data) {
        return;
    }
    s_mode = SR_MODE_PAUSED;
    s_last_vad_state = VAD_SILENCE;
    s_active_sleep_ignore_until = 0;
    ESP_LOGI(TAG, "sr standby: resetting AFE buffer");
    s_afe->reset_buffer(s_afe_data);
    ESP_LOGI(TAG, "sr standby: clearing feed tokens");
    clear_feed_tokens();
    ESP_LOGI(TAG, "sr standby: enabling WakeNet");
    s_afe->enable_wakenet(s_afe_data);
    ESP_LOGI(TAG, "sr standby: WakeNet enabled");
    s_mode = SR_MODE_STANDBY;
}

void speech_recognition_enter_active(void)
{
    if (!s_afe_data) {
        return;
    }
    s_mode = SR_MODE_PAUSED;
    s_last_vad_state = VAD_SILENCE;
    // Keep WakeNet running during the conversation: "Jarvis" is both the wake
    // word (standby) and the standby request (active).  Guard the first two
    // seconds so the wake utterance tail cannot immediately sleep the device.
    s_active_sleep_ignore_until =
        xTaskGetTickCount() + pdMS_TO_TICKS(ACTIVE_SLEEP_GUARD_MS);
    s_afe->reset_buffer(s_afe_data);
    clear_feed_tokens();
    s_afe->enable_wakenet(s_afe_data);
    ESP_LOGI(TAG, "active: Jarvis requests standby");
    s_mode = SR_MODE_ACTIVE;
}

void speech_recognition_pause(void)
{
    if (s_pcm_lock) {
        ESP_LOGI(TAG, "sr pause: waiting for PCM feed lock");
        xSemaphoreTake(s_pcm_lock, portMAX_DELAY);
        ESP_LOGI(TAG, "sr pause: PCM feed lock acquired");
    }
    s_mode = SR_MODE_PAUSED;
    s_last_vad_state = VAD_SILENCE;
    clear_feed_tokens();
    ESP_LOGI(TAG, "sr pause: done");
    if (s_pcm_lock) {
        xSemaphoreGive(s_pcm_lock);
    }
}

esp_err_t speech_recognition_set_detection_threshold(float threshold)
{
    if (!s_afe || !s_afe_data || threshold < 0.4f || threshold >= 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    // ESP-SR's AFE API returns 1 for success here (rather than ESP_OK/0).
    int result = s_afe->set_wakenet_threshold(s_afe_data, 1, threshold);
    if (result != 1) {
        ESP_LOGE(TAG, "set WakeNet threshold %.2f failed: %d", (double)threshold, result);
        return ESP_FAIL;
    }

    s_wake_threshold = threshold;
    ESP_LOGI(TAG, "WakeNet threshold set to %.2f", (double)threshold);
    return ESP_OK;
}

esp_err_t speech_recognition_set_wake_threshold(float threshold)
{
    return speech_recognition_set_detection_threshold(threshold);
}

float speech_recognition_get_wake_threshold(void)
{
    return s_wake_threshold;
}

uint32_t speech_recognition_get_capture_heartbeat(void)
{
    return s_capture_heartbeat;
}
