#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_capture_audio_src_if.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a 16 kHz mono microphone source that applies ESP-SR AEC using the
 * PCM sent to the SparkBot speaker as its far-end reference.
 */
esp_capture_audio_src_if_t *aec_audio_source_create(esp_codec_dev_handle_t record_handle);

/**
 * av_render/I2S callback.  It receives the rendered stereo PCM and retains a
 * mono far-end reference for the next microphone AEC block.
 */
int aec_audio_source_push_playback_reference(uint8_t *data, int size, void *context);

/**
 * Marks whether the remote renderer is currently producing audible speech.
 * This does not gate the reference path; it makes AEC timing diagnostics
 * meaningful and discards a stale far-end reference after a reply finishes.
 */
void aec_audio_source_set_far_end_active(bool active);

/**
 * Enables AEC processing only while a LiveKit conversation owns the
 * microphone.  Standby keeps the same source but passes raw PCM straight
 * through so WakeNet does not compete with AEC for CPU time.
 */
void aec_audio_source_set_enabled(bool enabled);

/** Drops stale far-end and microphone buffers when a LiveKit room closes. */
void aec_audio_source_reset(void);

#ifdef __cplusplus
}
#endif
