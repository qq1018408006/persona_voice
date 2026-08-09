#include "app_controller.h"

#include <stdint.h>

#include "board_audio.h"
#include "board_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "livekit_terminal.h"
#include "media_pipeline.h"
#include "network.h"
#include "nvs.h"
#include "speech_recognition.h"
#include "token_client.h"

static const char *TAG = "sparkbot_app";
static QueueHandle_t s_events;
static volatile app_state_t s_state = APP_STATE_BOOTING;
// Warm standby keeps the LiveKit room/capture pipeline alive, but closes the
// final RTP microphone gate. Cold standby is the old fully disconnected mode.
static bool s_warm_standby;
static bool s_boot_prewarm_pending;
// A local wake word heard while the boot prewarm room is still connecting.
// begin_wake() cannot run in APP_STATE_CONNECTING, so remember the request
// and activate directly when the room finishes instead of silently dropping
// the user's first "Jarvis".
static bool s_wake_pending_during_prewarm;
static uint32_t s_standby_heartbeat;
static uint32_t s_standby_stale_checks;

// When local recognition is returned to WakeNet, the last few capture blocks
// can still contain the tail of Jarvis's own speech / I2S reconfiguration.
// At high microphone sensitivity that tail can occasionally be classified as
// a fresh "Jarvis" and immediately undo a completed standby request.  Keep a
// short controller-side guard after entering standby.  It must stay short
// enough that a real wake phrase spoken 1-2 s after standby is accepted on
// the first try (the previous 3 s window ate intentional wake ups).
#define STANDBY_WAKE_SETTLE_MS 1000
static TickType_t s_ignore_local_wake_until;
// Detection tick of the most recent local wake word, captured when the event
// is posted.  The controller compares this (not the processing tick) against
// the settling deadline so a wake queued while the controller is temporarily
// busy cannot be misjudged as a standby tail just because it is processed
// late.
static volatile TickType_t s_last_wake_detected_at;

static void suppress_local_wake_for(TickType_t duration)
{
    s_ignore_local_wake_until = xTaskGetTickCount() + duration;
}

static bool local_wake_is_settling(TickType_t detected_at)
{
    // Signed subtraction handles the normal FreeRTOS tick counter rollover.
    return (int32_t)(detected_at - s_ignore_local_wake_until) < 0;
}

#define SENSITIVITY_NVS_NAMESPACE "sparkbot"
// Key bumped so existing devices pick up the new maximum-sensitivity default.
#define SENSITIVITY_NVS_KEY       "mic_sens2"
#define SENSITIVITY_LEVEL_COUNT   6
#define VOLUME_NVS_KEY            "volume"
#define VOLUME_LEVEL_COUNT        8

// Fixed speaker volume steps, 30%..100% (touch control disabled).
static const int s_volume_percent[VOLUME_LEVEL_COUNT] = {
    30, 40, 50, 60, 70, 80, 90, 100,
};

typedef struct {
    int mic_gain_db;
    float detection_threshold;
} sensitivity_profile_t;

static const sensitivity_profile_t s_sensitivity_profiles[SENSITIVITY_LEVEL_COUNT] = {
    { .mic_gain_db = 24, .detection_threshold = 0.68f },
    { .mic_gain_db = 29, .detection_threshold = 0.65f },
    { .mic_gain_db = 34, .detection_threshold = 0.62f },
    { .mic_gain_db = 39, .detection_threshold = 0.59f },
    { .mic_gain_db = 40, .detection_threshold = 0.56f },
    { .mic_gain_db = 42, .detection_threshold = 0.52f },
};
// Default to the maximum sensitivity (42 dB / 0.52).
static int s_sensitivity_level = SENSITIVITY_LEVEL_COUNT;

// Standby only needs the local "Jarvis" keyword, while the maximum analog
// gain exists for far-field conversation ASR.  In standby the AEC/NS/AGC
// enhancement is deliberately bypassed, so feeding WakeNet the same 48 dB
// hot signal (and the 0.52 threshold tuned for speech-to-text) turns ambient
// speech/noise into occasional self-wakes.  Use a moderately quieter standby
// input and a threshold above the ASR-tuned 0.52; the values below sit
// between the old false-wake-prone combo (48 dB / 0.52) and the over-cautious
// first attempt (39 dB / 0.60) so a normal "Jarvis" still wakes on the first
// try without making the environment trigger the device.
#define STANDBY_MIC_GAIN_DB      44
#define STANDBY_WAKE_THRESHOLD   0.58f

typedef struct {
    bool is_volume;
    int8_t value;
} nvs_save_job_t;
static QueueHandle_t s_nvs_save_queue;

// NVS writes program flash, which freezes the cache.  That is only safe when
// the calling task's stack lives in internal RAM; the controller task stack
// is in PSRAM (internal RAM is reserved for WiFi DMA), so all persistent
// writes are handed to this dedicated internal-stack worker.
static void nvs_save_worker(void *unused)
{
    (void)unused;
    nvs_save_job_t job;
    while (xQueueReceive(s_nvs_save_queue, &job, portMAX_DELAY) == pdTRUE) {
        nvs_handle_t nvs;
        if (nvs_open(SENSITIVITY_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
            ESP_LOGW(TAG, "cannot open NVS to persist %s",
                     job.is_volume ? "volume" : "sensitivity");
            continue;
        }
        esp_err_t result = job.is_volume
            ? nvs_set_i8(nvs, VOLUME_NVS_KEY, job.value)
            : nvs_set_i8(nvs, SENSITIVITY_NVS_KEY, job.value);
        if (result == ESP_OK) {
            result = nvs_commit(nvs);
        }
        nvs_close(nvs);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "cannot persist %s=%d: %s",
                     job.is_volume ? "volume" : "sensitivity",
                     job.value,
                     esp_err_to_name(result));
        }
    }
}

static void request_nvs_save(bool is_volume, int value)
{
    if (!s_nvs_save_queue) {
        return;
    }
    const nvs_save_job_t job = {
        .is_volume = is_volume,
        .value = (int8_t)value,
    };
    xQueueSend(s_nvs_save_queue, &job, 0);
}

static void apply_standby_wake_parameters(void)
{
    const sensitivity_profile_t *profile = &s_sensitivity_profiles[s_sensitivity_level - 1];
    const float standby_threshold =
        profile->detection_threshold > STANDBY_WAKE_THRESHOLD
        ? profile->detection_threshold
        : STANDBY_WAKE_THRESHOLD;
    if (board_audio_set_mic_gain_db(STANDBY_MIC_GAIN_DB) != ESP_OK ||
        speech_recognition_set_detection_threshold(standby_threshold) != ESP_OK) {
        ESP_LOGW(TAG, "standby wake parameters could not be fully applied");
        return;
    }
    ESP_LOGI(
        TAG,
        "standby wake parameters: mic_gain=%d dB detection_threshold=%.2f (WakeNet)",
        STANDBY_MIC_GAIN_DB,
        (double)standby_threshold);
}

static void apply_sensitivity_level(int level, bool persist)
{
    if (level < 1) {
        level = 1;
    } else if (level > SENSITIVITY_LEVEL_COUNT) {
        level = SENSITIVITY_LEVEL_COUNT;
    }
    const sensitivity_profile_t *profile = &s_sensitivity_profiles[level - 1];
    if (board_audio_set_mic_gain_db(profile->mic_gain_db) != ESP_OK ||
        speech_recognition_set_detection_threshold(profile->detection_threshold) != ESP_OK) {
        ESP_LOGW(TAG, "microphone sensitivity level %d could not be fully applied", level);
        return;
    }
    s_sensitivity_level = level;
    if (persist) {
        request_nvs_save(false, level);
    }
    board_ui_show_sensitivity(s_sensitivity_level, SENSITIVITY_LEVEL_COUNT);
    ESP_LOGI(
        TAG,
        "microphone sensitivity=%d/%d gain=%d dB detection_threshold=%.2f (WakeNet)",
        s_sensitivity_level,
        SENSITIVITY_LEVEL_COUNT,
        profile->mic_gain_db,
        (double)profile->detection_threshold);
    if (s_state == APP_STATE_STANDBY) {
        apply_standby_wake_parameters();
    }
}

static void restore_active_sensitivity(void)
{
    apply_sensitivity_level(s_sensitivity_level, false);
}

static void load_sensitivity_level(void)
{
    // Touch sensitivity control is disabled: always use the fixed maximum
    // profile (42 dB / 0.52), ignoring any previously saved NVS level.
    apply_sensitivity_level(SENSITIVITY_LEVEL_COUNT, false);
}

static int s_volume_level = 2; // 50%

static void apply_volume_level(int level, bool persist)
{
    if (level < 0) {
        level = 0;
    } else if (level >= VOLUME_LEVEL_COUNT) {
        level = VOLUME_LEVEL_COUNT - 1;
    }
    const int volume_percent = s_volume_percent[level];
    if (board_audio_set_volume(volume_percent) != ESP_OK) {
        ESP_LOGW(TAG, "speaker volume %d%% could not be applied", volume_percent);
        return;
    }
    s_volume_level = level;
    if (persist) {
        request_nvs_save(true, level);
    }
    board_ui_show_volume(s_volume_level + 1, VOLUME_LEVEL_COUNT);
    ESP_LOGI(TAG, "speaker volume=%d%% (%d/%d)", volume_percent, s_volume_level + 1, VOLUME_LEVEL_COUNT);
}

static void load_volume_level(void)
{
    int8_t saved_level = 2; // 50%
    nvs_handle_t nvs;
    if (nvs_open(SENSITIVITY_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t result = nvs_get_i8(nvs, VOLUME_NVS_KEY, &saved_level);
        nvs_close(nvs);
        if (result != ESP_OK) {
            saved_level = 2;
        }
    }
    if (saved_level < 0 || saved_level >= VOLUME_LEVEL_COUNT) {
        saved_level = 2;
    }
    apply_volume_level(saved_level, false);
}

// The ES8311 DAC volume register is only applied while the playback codec is
// open.  At boot the renderer is closed, so set_out_vol() is a no-op and the
// first sound would otherwise play at the codec default.  Re-assert the saved
// level at every point where the renderer (re)opens.
static void reassert_speaker_volume(void)
{
    if (board_audio_set_volume(s_volume_percent[s_volume_level]) != ESP_OK) {
        ESP_LOGW(
            TAG,
            "cannot reassert speaker volume %d%%",
            s_volume_percent[s_volume_level]);
    }
}

static void enter_standby(bool with_tone)
{
    s_warm_standby = false;
    s_boot_prewarm_pending = false;
    speech_recognition_pause();
    if (livekit_terminal_state() != LIVEKIT_CONNECTION_STATE_DISCONNECTED) {
        livekit_terminal_disconnect();
        for (int i = 0; i < 20; ++i) {
            if (livekit_terminal_state() == LIVEKIT_CONNECTION_STATE_DISCONNECTED) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // The Disconnected callback is emitted after LiveKit has stopped capture
    // and released its peers.  Clear its independent playback session before
    // reopening local PCM capture on the same I2S peripheral.
    if (media_pipeline_reset_remote_audio() != ESP_OK) {
        ESP_LOGW(TAG, "remote-audio reset failed while entering standby");
    }

    if (media_pipeline_start_local() != ESP_OK) {
        s_state = APP_STATE_ERROR;
        board_ui_set_state(BOARD_UI_ERROR);
        return;
    }
    speech_recognition_enter_standby();
    apply_standby_wake_parameters();
    suppress_local_wake_for(pdMS_TO_TICKS(STANDBY_WAKE_SETTLE_MS));
    s_state = APP_STATE_STANDBY;
    s_standby_heartbeat = speech_recognition_get_capture_heartbeat();
    s_standby_stale_checks = 0;
    board_ui_set_state(BOARD_UI_STANDBY);
    if (with_tone) {
        board_audio_play_tone(BOARD_TONE_STANDBY);
    }
    ESP_LOGI(TAG, "standby: local WakeNet only; LiveKit is disconnected");
}

static void enter_warm_standby(void)
{
    // Commit the standby state and UI before any potentially blocking media
    // work.  The voice agent has already finished its reply (sleep_ready),
    // so the user must see/hear standby immediately; pausing the shared
    // renderer below can stall for seconds if a queue is congested.
    if (livekit_terminal_set_microphone_uplink_muted(true) != ESP_OK) {
        ESP_LOGW(TAG, "cannot mute microphone uplink for warm standby");
    }
    suppress_local_wake_for(pdMS_TO_TICKS(STANDBY_WAKE_SETTLE_MS));
    s_warm_standby = true;
    s_boot_prewarm_pending = false;
    s_state = APP_STATE_STANDBY;
    // Connected warm standby: show the ready checkmark so the user knows the
    // room is already up and a wake word will start the conversation with
    // no reconnection delay.
    board_ui_set_state(BOARD_UI_READY);
    ESP_LOGI(TAG, "warm standby: LiveKit connected, microphone RTP muted");
    // The shared capture pipeline is still running and feeds the local PCM
    // sink, so WakeNet remains local while network audio is token-safe muted.
    ESP_LOGI(TAG, "warm standby: switching local recognition to WakeNet");
    speech_recognition_pause();
    speech_recognition_enter_standby();
    apply_standby_wake_parameters();
    ESP_LOGI(TAG, "warm standby: pausing remote renderer");
    if (media_pipeline_pause_remote_audio() != ESP_OK) {
        ESP_LOGW(TAG, "cannot pause renderer for warm standby");
    }
}

static void fail_wake(const char *reason)
{
    ESP_LOGE(TAG, "wake failed: %s", reason);
    s_state = APP_STATE_ERROR;
    board_ui_set_state(BOARD_UI_ERROR);
    board_audio_play_tone(BOARD_TONE_ERROR);
    vTaskDelay(pdMS_TO_TICKS(350));
    enter_standby(false);
}

static void prewarm_livekit_after_boot(void)
{
#if CONFIG_SPARKBOT_PREWARM_LIVEKIT
    // WakeNet is already running locally. Switch only its capture ownership
    // while the shared LiveKit path opens; no mic packets are unmuted here.
    // Pause the PCM feed first: pcm_feed_task must not touch the capture
    // sink while media_pipeline_stop_local() tears the GMF path down (a
    // race there hit a destroyed data queue and aborted at boot).
    speech_recognition_pause();
    if (media_pipeline_stop_local() != ESP_OK ||
        media_pipeline_prepare_livekit() != ESP_OK ||
        network_wait_connected(15000) != ESP_OK) {
        ESP_LOGW(TAG, "boot prewarm unavailable; remaining in cold standby");
        enter_standby(false);
        return;
    }
    // Set the RTP gate before connecting. The room may begin its capture
    // path immediately after ICE completes, so waiting for the Connected
    // callback would leave a short non-standby audio window at boot.
    if (livekit_terminal_set_microphone_uplink_muted(true) != ESP_OK) {
        ESP_LOGW(TAG, "cannot pre-mute microphone before boot prewarm");
    }
    livekit_credentials_t credentials;
    if (token_client_fetch(&credentials) != ESP_OK ||
        livekit_terminal_connect(&credentials) != ESP_OK) {
        ESP_LOGW(TAG, "boot LiveKit prewarm failed; remaining in cold standby");
        enter_standby(false);
        return;
    }
    s_boot_prewarm_pending = true;
    s_state = APP_STATE_CONNECTING;
    board_ui_set_state(BOARD_UI_CONNECTING);
    ESP_LOGI(TAG, "boot prewarm requested; waiting for token-safe warm standby");
#endif
}

static void cancel_sleep_drain(void)
{
    if (s_state != APP_STATE_SLEEP_DRAINING) {
        return;
    }

    // A moon/draining state intentionally keeps the room and renderer alive.
    // Reuse that room rather than reconnecting: restore the RTP uplink, return
    // local recognition to active mode, and tell the runtime not to emit its
    // already-pending sleep_ready acknowledgement.
    s_state = APP_STATE_ACTIVE;
    if (livekit_terminal_set_microphone_uplink_muted(false) != ESP_OK) {
        ESP_LOGW(TAG, "cannot restore microphone uplink while cancelling standby");
    }
    reassert_speaker_volume();
    restore_active_sensitivity();
    speech_recognition_enter_active();
    board_ui_set_state(BOARD_UI_LISTENING);
    if (livekit_terminal_send_sleep_cancel() != ESP_OK) {
        ESP_LOGW(TAG, "unable to notify voice agent that orderly standby was cancelled");
    }
    ESP_LOGI(TAG, "standby drain cancelled by local wake word; conversation resumed");
}

static void begin_wake(void)
{
    if (s_state == APP_STATE_SLEEP_DRAINING) {
        cancel_sleep_drain();
        return;
    }
    if (s_state != APP_STATE_STANDBY) {
        return;
    }

    // The room must not be Connected while the app is in cold standby: the
    // local capture would still own the microphone and the room would have
    // no uplink.  Close such a stray connection so the cold path below can
    // start from a clean disconnected room.
    if (!s_warm_standby &&
        livekit_terminal_state() == LIVEKIT_CONNECTION_STATE_CONNECTED) {
        ESP_LOGW(TAG, "wake: room connected outside warm standby; closing it for a clean cold wake");
        livekit_terminal_disconnect();
        for (int i = 0;
             i < 20 &&
             livekit_terminal_state() != LIVEKIT_CONNECTION_STATE_DISCONNECTED;
             ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (s_warm_standby &&
        livekit_terminal_state() == LIVEKIT_CONNECTION_STATE_CONNECTED) {
        s_state = APP_STATE_ACTIVE;
        speech_recognition_pause();
        if (media_pipeline_resume_remote_audio() != ESP_OK) {
            fail_wake("cannot resume warm standby renderer");
            return;
        }
        reassert_speaker_volume();
        if (livekit_terminal_set_microphone_uplink_muted(false) != ESP_OK) {
            fail_wake("cannot restore warm standby microphone uplink");
            return;
        }
        s_warm_standby = false;
        restore_active_sensitivity();
        speech_recognition_enter_active();
        board_ui_set_state(BOARD_UI_LISTENING);
        if (livekit_terminal_send_wake_event() != ESP_OK) {
            ESP_LOGW(TAG, "unable to notify voice agent of warm wake");
        }
        ESP_LOGI(TAG, "warm wake accepted; reused LiveKit room without reconnect");
        return;
    }
    s_state = APP_STATE_CONNECTING;
    speech_recognition_pause();
    board_ui_set_state(BOARD_UI_CONNECTING);
    board_audio_play_tone(BOARD_TONE_WAKE_DETECTED);

    if (media_pipeline_stop_local() != ESP_OK) {
        fail_wake("cannot hand microphone capture to LiveKit");
        return;
    }
    if (media_pipeline_prepare_livekit() != ESP_OK) {
        fail_wake("cannot enable microphone uplink for LiveKit");
        return;
    }
    reassert_speaker_volume();
    // A previous orderly standby mutes at the final RTP boundary.  The room
    // object is retained across reconnects, so explicitly reopen it before
    // this new live conversation begins.
    if (livekit_terminal_set_microphone_uplink_muted(false) != ESP_OK) {
        ESP_LOGW(TAG, "cannot restore microphone uplink before wake");
    }
    if (network_wait_connected(15000) != ESP_OK) {
        fail_wake("Wi-Fi unavailable");
        return;
    }

    livekit_credentials_t credentials;
    if (token_client_fetch(&credentials) != ESP_OK) {
        fail_wake("short-lived token request failed");
        return;
    }
    if (livekit_terminal_connect(&credentials) != ESP_OK) {
        fail_wake("LiveKit connect request failed");
        return;
    }
    ESP_LOGI(TAG, "wake accepted; waiting for LiveKit media connection");
}

static void begin_sleep(void)
{
    if (s_state == APP_STATE_STANDBY || s_state == APP_STATE_SLEEP_DRAINING) {
        return;
    }

    // Discard all subsequent RTP microphone frames. The LiveKit room and
    // renderer deliberately remain alive: Gemini can finish a current reply
    // and a Hermes/Codex result can still be spoken before the agent
    // authorizes final standby. Local recognition is immediately re-enabled
    // in standby mode below, but it accepts only a new wake word so this
    // draining state remains reversible without consuming Gemini tokens.
    speech_recognition_pause();
    if (livekit_terminal_state() == LIVEKIT_CONNECTION_STATE_CONNECTED) {
        if (livekit_terminal_set_microphone_uplink_muted(true) != ESP_OK) {
            ESP_LOGW(TAG, "cannot mute microphone uplink for orderly standby");
        }
        if (livekit_terminal_send_sleep_request() == ESP_OK) {
            s_state = APP_STATE_SLEEP_DRAINING;
            // Keep an entirely local “Jarvis” escape hatch while draining:
            // the controller maps a standby wake word to cancel_sleep_drain.
            speech_recognition_enter_standby();
            apply_standby_wake_parameters();
            board_ui_set_state(BOARD_UI_SLEEP_DRAINING);
            ESP_LOGI(
                TAG,
                "standby requested: microphone uplink closed; local wake remains available while draining");
            return;
        }
        ESP_LOGW(TAG, "agent standby request failed; falling back to immediate disconnect");
    }

    // A room that is still connecting (or whose data channel failed) has no
    // safe way to report completion, so retain the former immediate fallback.
    if (media_pipeline_pause_remote_audio() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    s_state = APP_STATE_DISCONNECTING;
    board_ui_set_state(BOARD_UI_CONNECTING);
    if (livekit_terminal_disconnect() != ESP_OK ||
        livekit_terminal_state() == LIVEKIT_CONNECTION_STATE_DISCONNECTED) {
        enter_standby(true);
    }
}

static void controller_task(void *unused)
{
    (void)unused;
    load_sensitivity_level();
    load_volume_level();
#if CONFIG_SPARKBOT_PREWARM_LIVEKIT
    // Boot straight into the prewarm connection: the screen shows the
    // connecting spinner while WiFi/token/room setup happens, and the ready
    // checkmark appears when warm standby is established.  A wake word heard
    // during connecting is remembered and activates as soon as the room is
    // up.  If prewarm fails it falls back to cold standby below.
    prewarm_livekit_after_boot();
#else
    enter_standby(false);
#endif

    while (true) {
        app_event_t event;
        if (xQueueReceive(s_events, &event, pdMS_TO_TICKS(1000)) != pdTRUE) {
            // Standby capture watchdog: while warm-stanbding, capture is owned
            // by the LiveKit room.  If the room drops (server close, token
            // expiry) the SDK may stop the capturer without a room-state
            // callback, leaving WakeNet deaf.  Detect a stalled heartbeat and
            // force a cold-standby restart of the local capture pipeline.
            if (s_state == APP_STATE_STANDBY) {
                const uint32_t heartbeat =
                    speech_recognition_get_capture_heartbeat();
                if (heartbeat != s_standby_heartbeat) {
                    s_standby_heartbeat = heartbeat;
                    s_standby_stale_checks = 0;
                } else if (++s_standby_stale_checks >= 3) {
                    ESP_LOGW(
                        TAG,
                        "standby capture stalled (%u s); restarting local recognition",
                        s_standby_stale_checks);
                    enter_standby(false);
                    s_standby_heartbeat =
                        speech_recognition_get_capture_heartbeat();
                    s_standby_stale_checks = 0;
                }
            }
            continue;
        }
        ESP_LOGI(TAG, "event=%d state=%d", event, s_state);
        switch (event) {
            case APP_EVENT_WAKE_WORD:
                if (s_state == APP_STATE_STANDBY &&
                    local_wake_is_settling(s_last_wake_detected_at)) {
                    // Drop a stale recognition result produced while entering
                    // standby and clear AFE history once more before accepting
                    // the next intentional wake phrase.
                    speech_recognition_enter_standby();
                    ESP_LOGI(TAG, "ignored local wake during standby settling window");
                    break;
                }
                if (s_state == APP_STATE_CONNECTING && s_boot_prewarm_pending) {
                    // The room is still connecting for boot prewarm.  Activate
                    // when it finishes instead of dropping this wake.
                    s_wake_pending_during_prewarm = true;
                    ESP_LOGI(TAG, "wake requested during boot prewarm; will activate on connect");
                    break;
                }
                begin_wake();
                break;
            case APP_EVENT_SLEEP_WORD:
                begin_sleep();
                break;
            case APP_EVENT_BUTTON_TOGGLE:
                if (s_state == APP_STATE_STANDBY) {
                    begin_wake();
                } else if (s_state == APP_STATE_SLEEP_DRAINING) {
                    cancel_sleep_drain();
                } else {
                    begin_sleep();
                }
                break;
            case APP_EVENT_BUTTON_LONG_PRESS:
                // Local speaker self-test: a long BOOT press plays the ready
                // double beep so DAC/PA health can be checked independently
                // of the remote Gemini audio path.
                if (board_audio_play_tone(BOARD_TONE_READY) != ESP_OK) {
                    ESP_LOGW(TAG, "local tone self-test failed");
                }
                break;
            case APP_EVENT_LIVEKIT_CONNECTED:
                if (s_state == APP_STATE_STANDBY && !s_warm_standby) {
                    // The room reconnected behind the app's back (for example
                    // after the standby watchdog restarted local capture).
                    // A connected room in cold standby would own a dead
                    // uplink; close it and stay in local WakeNet standby.
                    ESP_LOGW(
                        TAG,
                        "unexpected room connect during cold standby; closing it");
                    livekit_terminal_disconnect();
                    break;
                }
                if (s_state == APP_STATE_CONNECTING) {
                    if (s_boot_prewarm_pending) {
                        s_boot_prewarm_pending = false;
                        if (s_wake_pending_during_prewarm) {
                            s_wake_pending_during_prewarm = false;
                            s_state = APP_STATE_ACTIVE;
                            s_warm_standby = false;
                            speech_recognition_pause();
                            if (media_pipeline_resume_remote_audio() != ESP_OK) {
                                fail_wake("cannot resume prewarm renderer");
                                break;
                            }
                            reassert_speaker_volume();
                            if (livekit_terminal_set_microphone_uplink_muted(false) != ESP_OK) {
                                ESP_LOGW(TAG, "cannot restore microphone uplink after prewarm wake");
                            }
                            restore_active_sensitivity();
                            speech_recognition_enter_active();
                            board_ui_set_state(BOARD_UI_LISTENING);
                            if (livekit_terminal_send_wake_event() != ESP_OK) {
                                ESP_LOGW(TAG, "unable to notify voice agent of prewarm wake");
                            }
                            ESP_LOGI(TAG, "prewarm wake accepted; activated on room connect");
                        } else {
                            enter_warm_standby();
                        }
                        break;
                    }
                    s_state = APP_STATE_ACTIVE;
                    restore_active_sensitivity();
                    speech_recognition_enter_active();
                    board_ui_set_state(BOARD_UI_LISTENING);
                    // Do not play a local “ready” tone here.  av_render has
                    // already opened the same ES8311 playback endpoint for
                    // remote LiveKit audio, while board_audio_play_tone()
                    // closes that endpoint when its tone finishes.  That
                    // would leave the renderer with a stale/closed device
                    // and make the first Gemini reply fail with 0x103.
                    // The wake tone was already played before LiveKit
                    // connected; the proactive voice reply is the audible
                    // ready indication once the room is active.
                    if (livekit_terminal_send_wake_event() != ESP_OK) {
                        ESP_LOGW(TAG, "unable to notify the voice agent of the local wake word");
                    }
                    ESP_LOGI(TAG, "active: LiveKit audio and local Jarvis wake/sleep toggle enabled");
                }
                break;
            case APP_EVENT_LIVEKIT_DISCONNECTED:
                if (s_state != APP_STATE_STANDBY || s_warm_standby) {
                    enter_standby(
                        s_state == APP_STATE_DISCONNECTING ||
                        s_state == APP_STATE_SLEEP_DRAINING);
                }
                break;
            case APP_EVENT_LIVEKIT_FAILED:
                fail_wake("LiveKit connection failed");
                break;
            case APP_EVENT_REMOTE_AUDIO_STARTED:
                // Playback begins: re-assert the saved volume and read the
                // DAC register back.  Every wake path must end with the
                // correct register value; this write+readback makes any
                // stale-volume problem visible in the serial log.
                reassert_speaker_volume();
                int dac_volume_reg = 0;
                const int dac_read =
                    esp_codec_dev_read_reg(
                        board_audio_playback_handle(),
                        0x32,
                        &dac_volume_reg);
                ESP_LOGI(
                    TAG,
                    "playback start: volume=%d%% dac_reg32=0x%02X read=%d",
                    s_volume_percent[s_volume_level],
                    dac_volume_reg,
                    dac_read);
                board_audio_log_audio_state("at playback start");
                if (s_state == APP_STATE_ACTIVE || s_state == APP_STATE_SLEEP_DRAINING) {
                    board_ui_set_state(BOARD_UI_SPEAKING);
                }
                break;
            case APP_EVENT_REMOTE_AUDIO_ENDED:
                if (s_state == APP_STATE_ACTIVE) {
                    board_ui_set_state(BOARD_UI_LISTENING);
                } else if (s_state == APP_STATE_SLEEP_DRAINING) {
                    // No microphone input is accepted while draining; return
                    // to the dedicated sleep-draining indicator rather than
                    // a misleading listening waveform.
                    board_ui_set_state(BOARD_UI_SLEEP_DRAINING);
                }
                break;
            case APP_EVENT_SLEEP_DRAIN_COMPLETE:
                if (s_state == APP_STATE_SLEEP_DRAINING) {
                    if (livekit_terminal_state() == LIVEKIT_CONNECTION_STATE_CONNECTED) {
                        ESP_LOGI(TAG, "agent drain complete; keeping warm token-safe standby");
                        enter_warm_standby();
                    } else {
                        ESP_LOGI(TAG, "agent drain complete after disconnect; entering cold standby");
                        enter_standby(true);
                    }
                }
                break;
        }
    }
}

esp_err_t app_controller_start(void)
{
    s_events = xQueueCreate(12, sizeof(app_event_t));
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
    s_nvs_save_queue = xQueueCreate(8, sizeof(nvs_save_job_t));
    if (!s_nvs_save_queue) {
        return ESP_ERR_NO_MEM;
    }
    // Small internal-RAM stack: this task only runs NVS writes, which need
    // an internal stack because flash writes freeze the cache (PSRAM becomes
    // unreachable while frozen).
    BaseType_t nvs_result = xTaskCreatePinnedToCoreWithCaps(
        nvs_save_worker,
        "sparkbot_nvs",
        3072,
        NULL,
        4,
        NULL,
        0,
        MALLOC_CAP_INTERNAL);
    if (nvs_result != pdPASS) {
        ESP_LOGE(TAG, "unable to start NVS save worker; settings will not persist");
    }
    // Keep the 8 KiB controller stack in PSRAM: internal RAM is shared with
    // the Wi-Fi DMA pool, ESP-SR and the LiveKit engine task, and another
    // internal stack allocation after livekit_terminal_init() fails.
    BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        controller_task,
        "sparkbot_controller",
        8192,
        NULL,
        5,
        NULL,
        0,
        MALLOC_CAP_SPIRAM);
    return result == pdPASS ? ESP_OK : ESP_FAIL;
}

bool app_controller_post(app_event_t event)
{
    if (event == APP_EVENT_WAKE_WORD) {
        s_last_wake_detected_at = xTaskGetTickCount();
    }
    return s_events && xQueueSend(s_events, &event, 0) == pdTRUE;
}

app_state_t app_controller_get_state(void)
{
    return s_state;
}
