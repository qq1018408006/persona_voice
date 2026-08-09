#pragma once

#include <stdint.h>
#include "av_render.h"
#include "esp_capture.h"
#include "esp_capture_sink.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*media_remote_audio_callback_t)(bool speaking);

esp_err_t media_pipeline_init(void);
esp_capture_handle_t media_pipeline_capture(void);
av_render_handle_t media_pipeline_renderer(void);
esp_capture_sink_handle_t media_pipeline_speech_sink(void);
void media_pipeline_set_remote_audio_callback(media_remote_audio_callback_t callback);

// I2S renderer callback. It derives remote-speaking state from real PCM
// energy and feeds the exact far-end frame to ESP-SR AEC before the same
// frame is written to the speaker.
int media_pipeline_on_playback_frame(uint8_t *data, int size, void *context);

/**
 * Release decoder/render tasks left by a disconnected LiveKit session while
 * retaining the renderer handle for the next session.
 */
esp_err_t media_pipeline_reset_remote_audio(void);
esp_err_t media_pipeline_pause_remote_audio(void);
// Resume a renderer deliberately paused for token-safe warm standby without
// tearing down the already-connected LiveKit room.
esp_err_t media_pipeline_resume_remote_audio(void);

esp_err_t media_pipeline_start_local(void);
esp_err_t media_pipeline_stop_local(void);

// Re-enable the preconfigured primary Opus capture path before a LiveKit room
// connects.  Standby deliberately disables that path so its unconsumed queue
// cannot block the always-on local PCM recognizer.
esp_err_t media_pipeline_prepare_livekit(void);

#ifdef __cplusplus
}
#endif
