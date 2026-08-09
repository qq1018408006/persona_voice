#pragma once

#include <stdbool.h>

#include "esp_capture_sink.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*speech_event_callback_t)(void);

esp_err_t speech_recognition_start(
    esp_capture_sink_handle_t pcm_sink,
    speech_event_callback_t on_wake,
    speech_event_callback_t on_sleep);
void speech_recognition_enter_standby(void);
void speech_recognition_enter_active(void);
void speech_recognition_pause(void);
esp_err_t speech_recognition_set_detection_threshold(float threshold);
// Backward-compatible name used by older controller code.  It updates the
// dedicated WakeNet threshold.
esp_err_t speech_recognition_set_wake_threshold(float threshold);
float speech_recognition_get_wake_threshold(void);
// Monotonic counter advanced on every successful capture-frame acquire.
// The controller uses it as a standby liveness heartbeat.
uint32_t speech_recognition_get_capture_heartbeat(void);

#ifdef __cplusplus
}
#endif
