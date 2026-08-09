#include "aec_audio_source.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "board_audio.h"
#include "esp_aec.h"
#include "esp_agc.h"
#include "esp_capture_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ns.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "sparkbot_aec";

// ESP-SR's packaged AEC processes fixed 32 ms blocks at 16 kHz.
#define AEC_SAMPLE_RATE              16000
#define AEC_BLOCK_SAMPLES            512
#define AEC_REFERENCE_DELAY_SAMPLES  0    // callback runs immediately before I2S write
#define AEC_REFERENCE_FIFO_SAMPLES   4096 // 256 ms
#define AEC_OUTPUT_FIFO_SAMPLES      1024
#define AEC_MAX_READ_SAMPLES         512

// The SparkBot speaker is centimetres from the microphone.  Duck the analog
// mic gain while a reply is audible so the echo stays below ADC clipping
// (clipped echo is non-linear and cannot be cancelled), then restore the
// conversation gain afterwards.
#define AEC_FAR_END_MIC_GAIN_DB      36
// Between replies use a raised analog gain.  The user talks from ~1 m away,
// and the digital AGC below normalizes the remaining distance loss; NS keeps
// the extra ambient noise from turning into false speech.
#define AEC_ACTIVE_MIC_GAIN_DB       42
// VOIP_LOW_COST removes the most speaker residual, giving Gemini the cleanest
// possible uplink.  There is no device-local wake-word/stop detector anymore,
// so preserving near-end energy during double-talk is no longer required.
#define AEC_MODE                      AEC_MODE_VOIP_LOW_COST
// Keep the ESP-SR recommended 4-tap filter: an 8-tap filter made aec_process
// so heavy that CPU1 starved the idle task past the watchdog deadline.  The
// echo path is kept linear (and thus cancellable at 4 taps) by the playback
// mic gain duck and the reduced speaker volume.
#define AEC_FILTER_LENGTH             4

// Far-field uplink enhancement: NS removes stationary room/fan noise (the
// biggest source of distant-speech ASR errors) and digital AGC normalizes
// quiet speech to a consistent level before Opus encoding.  Both process
// 10 ms frames; a 512-sample AEC block is 3 full frames plus 32 samples.
#define UPLINK_NS_MODE                 0 // 0 mild, 1 medium, 2 aggressive
#define UPLINK_AGC_MODE                AGC_MODE_2
#define UPLINK_AGC_TARGET_DBFS         (-6)
// Compression gain in dB for AGC_MODE_2/3 (default 8).  0 essentially disables
// the boost, which made quiet 1 m speech too soft for Gemini; 24 dB combined
// with the -3 dBFS limiter recovered distance loss but drove normal speech
// into clipping/compression, which Gemini hears as distorted ASR input.
// 12 dB with a -6 dBFS target keeps quiet speech audible while leaving
// enough headroom for loud/nearby speech to stay clean.
#define UPLINK_AGC_COMPRESSION_GAIN_DB 12
#define UPLINK_ENHANCE_FRAME_SAMPLES   160 // 10 ms at 16 kHz

typedef struct {
    esp_capture_audio_src_if_t base;
    esp_codec_dev_handle_t record_handle;
    esp_capture_audio_info_t info;
    bool open;
    bool started;
    bool fixed_caps;
    volatile bool processing_enabled;

    aec_handle_t *aec;
    int16_t *mic_block;
    int16_t *ref_block;
    int16_t *aec_block;
    int16_t *read_buffer;
    int16_t *enhance_buf;
    ns_handle_t ns;
    void *agc;
    size_t mic_count;

    int16_t output_fifo[AEC_OUTPUT_FIFO_SAMPLES];
    size_t output_read;
    size_t output_count;

    int16_t reference_fifo[AEC_REFERENCE_FIFO_SAMPLES];
    size_t reference_read;
    size_t reference_write;
    size_t reference_count;
    portMUX_TYPE reference_lock;

    volatile bool far_end_active;
    int saved_mic_gain_db;
    bool mic_gain_ducked;

    // Diagnostics are intentionally split into a capture-side view and a
    // renderer-side view.  The old aggregate counter was dominated by silent
    // periods, so it could make a healthy renderer look like an AEC failure.
    uint32_t far_end_blocks;
    uint32_t far_end_referenced_blocks;
    uint32_t far_end_reference_missing;
    uint32_t reference_pushes;
    uint32_t reference_samples;
    uint32_t reference_non_silent_samples;
    uint32_t reference_fifo_high_water;
    uint32_t reference_overflows;
    uint32_t log_blocks;
    // Post-AEC output level over the same diagnostic window.  If this is
    // near zero while the far-end reference is loud, ESP-SR AEC is cancelling
    // too much (or failing), which would explain stop phrases being missed
    // exactly while Jarvis is speaking.
    uint32_t post_aec_peak;
    uint64_t post_aec_sum;
    uint32_t post_aec_samples;
} aec_audio_source_t;

static aec_audio_source_t *s_source;

static inline int16_t *aligned_pcm_alloc(size_t samples)
{
    return heap_caps_aligned_alloc(
        16,
        samples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void clear_processing_buffers(aec_audio_source_t *src)
{
    src->mic_count = 0;
    src->output_read = 0;
    src->output_count = 0;
    taskENTER_CRITICAL(&src->reference_lock);
    src->reference_read = 0;
    src->reference_write = 0;
    src->reference_count = 0;
    src->reference_pushes = 0;
    src->reference_samples = 0;
    src->reference_non_silent_samples = 0;
    src->reference_fifo_high_water = 0;
    src->reference_overflows = 0;
    taskEXIT_CRITICAL(&src->reference_lock);
    src->post_aec_peak = 0;
    src->post_aec_sum = 0;
    src->post_aec_samples = 0;
    src->far_end_blocks = 0;
    src->far_end_referenced_blocks = 0;
    src->far_end_reference_missing = 0;
    src->log_blocks = 0;
}

static esp_capture_err_t source_open(esp_capture_audio_src_if_t *base)
{
    aec_audio_source_t *src = (aec_audio_source_t *)base;
    if (!src->record_handle || !src->aec) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    src->open = true;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_get_codecs(
    esp_capture_audio_src_if_t *base,
    const esp_capture_format_id_t **codecs,
    uint8_t *num)
{
    (void)base;
    static const esp_capture_format_id_t codecs_supported[] = { ESP_CAPTURE_FMT_ID_PCM };
    if (!codecs || !num) {
        return ESP_CAPTURE_ERR_INVALID_ARG;
    }
    *codecs = codecs_supported;
    *num = 1;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_set_fixed_caps(
    esp_capture_audio_src_if_t *base,
    const esp_capture_audio_info_t *caps)
{
    if (!base || !caps || caps->format_id != ESP_CAPTURE_FMT_ID_PCM ||
        caps->sample_rate != AEC_SAMPLE_RATE || caps->channel != 1 ||
        caps->bits_per_sample != 16) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    aec_audio_source_t *src = (aec_audio_source_t *)base;
    if (src->started) {
        return ESP_CAPTURE_ERR_INVALID_STATE;
    }
    src->info = *caps;
    src->fixed_caps = true;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_negotiate_caps(
    esp_capture_audio_src_if_t *base,
    esp_capture_audio_info_t *input,
    esp_capture_audio_info_t *output)
{
    if (!base || !input || !output || input->format_id != ESP_CAPTURE_FMT_ID_PCM ||
        input->sample_rate != AEC_SAMPLE_RATE || input->channel != 1 ||
        input->bits_per_sample != 16) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    aec_audio_source_t *src = (aec_audio_source_t *)base;
    src->info = *input;
    *output = src->info;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_start(esp_capture_audio_src_if_t *base)
{
    aec_audio_source_t *src = (aec_audio_source_t *)base;
    if (!src->open || !src->aec) {
        return ESP_CAPTURE_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = src->info.sample_rate,
        .bits_per_sample = src->info.bits_per_sample,
        .channel = src->info.channel,
    };
    if (esp_codec_dev_open(src->record_handle, &sample_info) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "unable to open microphone codec for AEC");
        return ESP_CAPTURE_ERR_INTERNAL;
    }
    clear_processing_buffers(src);
    src->started = true;
    ESP_LOGI(TAG, "full-duplex AEC source started: 16 kHz, 32 ms blocks, reference delay=%d ms",
        AEC_REFERENCE_DELAY_SAMPLES * 1000 / AEC_SAMPLE_RATE);
    return ESP_CAPTURE_ERR_OK;
}

static void output_push(aec_audio_source_t *src, const int16_t *samples, size_t count)
{
    // In steady state input and output are equal.  If a future pipeline asks
    // for an unusual frame size, preserve the newest cleaned audio rather
    // than blocking the real-time capture task.
    while (count-- > 0) {
        if (src->output_count == AEC_OUTPUT_FIFO_SAMPLES) {
            src->output_read = (src->output_read + 1) % AEC_OUTPUT_FIFO_SAMPLES;
            src->output_count--;
        }
        size_t write = (src->output_read + src->output_count) % AEC_OUTPUT_FIFO_SAMPLES;
        src->output_fifo[write] = *samples++;
        src->output_count++;
    }
}

static void enhance_uplink_block(aec_audio_source_t *src)
{
    if (!src->ns || !src->agc || !src->enhance_buf) {
        return;
    }
    size_t offset = 0;
    while (offset + UPLINK_ENHANCE_FRAME_SAMPLES <= AEC_BLOCK_SAMPLES) {
        ns_process(src->ns, src->aec_block + offset, src->enhance_buf);
        esp_agc_process(
            src->agc,
            src->enhance_buf,
            src->aec_block + offset,
            UPLINK_ENHANCE_FRAME_SAMPLES,
            AEC_SAMPLE_RATE);
        offset += UPLINK_ENHANCE_FRAME_SAMPLES;
    }
    // The trailing 32 samples (2 ms) are passed through unchanged; they are
    // too short for a 10 ms NS/AGC frame and the discontinuity is inaudible.
}

static bool reference_pop_block(aec_audio_source_t *src)
{
    bool available = false;
    taskENTER_CRITICAL(&src->reference_lock);
    // Renderer reference is supplied immediately before the corresponding
    // I2S write.  AEC tracks the small codec/acoustic delay internally, so
    // consume the same render period rather than adding a full 32 ms delay.
    if (src->reference_count >= AEC_BLOCK_SAMPLES + AEC_REFERENCE_DELAY_SAMPLES) {
        for (size_t i = 0; i < AEC_BLOCK_SAMPLES; ++i) {
            src->ref_block[i] = src->reference_fifo[src->reference_read];
            src->reference_read = (src->reference_read + 1) % AEC_REFERENCE_FIFO_SAMPLES;
        }
        src->reference_count -= AEC_BLOCK_SAMPLES;
        available = true;
    }
    taskEXIT_CRITICAL(&src->reference_lock);
    if (!available) {
        memset(src->ref_block, 0, AEC_BLOCK_SAMPLES * sizeof(int16_t));
    }
    return available;
}

static void process_aec_block(aec_audio_source_t *src)
{
    // The renderer keeps its transport clock alive with silent PCM between
    // replies.  Feeding those zero/stale blocks into AEC can attenuate a
    // perfectly valid near-end user utterance after Jarvis has stopped.  Only
    // apply the adaptive filter during an actual far-end speech window.
    const bool far_end_active = src->far_end_active;
    const bool have_reference = far_end_active && reference_pop_block(src);
    if (have_reference) {
        aec_process(src->aec, src->mic_block, src->ref_block, src->aec_block);
    } else {
        // No far-end PCM means Jarvis is silent.  Passing a zero reference to
        // the adaptive filter can colour near-end speech during its warm-up;
        // preserve the microphone samples exactly until a real renderer frame
        // establishes the full-duplex reference clock.
        memcpy(src->aec_block, src->mic_block, AEC_BLOCK_SAMPLES * sizeof(int16_t));
    }
    for (size_t i = 0; i < AEC_BLOCK_SAMPLES; ++i) {
        const int32_t sample = src->aec_block[i];
        const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        src->post_aec_sum += magnitude;
        if (magnitude > src->post_aec_peak) {
            src->post_aec_peak = magnitude;
        }
    }
    src->post_aec_samples += AEC_BLOCK_SAMPLES;
    enhance_uplink_block(src);
    output_push(src, src->aec_block, AEC_BLOCK_SAMPLES);
    if (far_end_active) {
        src->far_end_blocks++;
        if (have_reference) {
            src->far_end_referenced_blocks++;
        } else {
            src->far_end_reference_missing++;
        }
    }
    if (++src->log_blocks >= 64) {
        uint32_t reference_pushes;
        uint32_t reference_samples;
        uint32_t reference_non_silent_samples;
        uint32_t reference_fifo_high_water;
        uint32_t reference_overflows;
        taskENTER_CRITICAL(&src->reference_lock);
        reference_pushes = src->reference_pushes;
        reference_samples = src->reference_samples;
        reference_non_silent_samples = src->reference_non_silent_samples;
        reference_fifo_high_water = src->reference_fifo_high_water;
        reference_overflows = src->reference_overflows;
        src->reference_pushes = 0;
        src->reference_samples = 0;
        src->reference_non_silent_samples = 0;
        src->reference_fifo_high_water = 0;
        src->reference_overflows = 0;
        taskEXIT_CRITICAL(&src->reference_lock);

        const uint32_t post_aec_mean = src->post_aec_samples
            ? (uint32_t)(src->post_aec_sum / src->post_aec_samples)
            : 0;
        if (src->far_end_blocks != 0) {
            ESP_LOGI(
                TAG,
                "AEC far-end 2 s: capture_blocks=%" PRIu32
                " ref_blocks=%" PRIu32 " missing=%" PRIu32
                " ref_pushes=%" PRIu32 " ref_ms=%" PRIu32
                " audible_ref_ms=%" PRIu32 " fifo_high_ms=%" PRIu32
                " overflows=%" PRIu32 " delay_ms=%d"
                " post_mean=%" PRIu32 " post_peak=%" PRIu32,
                src->far_end_blocks,
                src->far_end_referenced_blocks,
                src->far_end_reference_missing,
                reference_pushes,
                reference_samples * 1000 / AEC_SAMPLE_RATE,
                reference_non_silent_samples * 1000 / AEC_SAMPLE_RATE,
                reference_fifo_high_water * 1000 / AEC_SAMPLE_RATE,
                reference_overflows,
                AEC_REFERENCE_DELAY_SAMPLES * 1000 / AEC_SAMPLE_RATE,
                post_aec_mean,
                src->post_aec_peak);
        }
        src->post_aec_peak = 0;
        src->post_aec_sum = 0;
        src->post_aec_samples = 0;
        src->far_end_blocks = 0;
        src->far_end_referenced_blocks = 0;
        src->far_end_reference_missing = 0;
        src->log_blocks = 0;
    }
}

static esp_capture_err_t source_read_frame(
    esp_capture_audio_src_if_t *base,
    esp_capture_stream_frame_t *frame)
{
    aec_audio_source_t *src = (aec_audio_source_t *)base;
    if (!src->started || !frame || !frame->data || frame->size == 0 ||
        frame->size % sizeof(int16_t) != 0) {
        return ESP_CAPTURE_ERR_INVALID_ARG;
    }
    const size_t samples = frame->size / sizeof(int16_t);
    if (samples > AEC_MAX_READ_SAMPLES) {
        ESP_LOGE(TAG, "unsupported capture frame: %u samples", (unsigned)samples);
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    if (esp_codec_dev_read(src->record_handle, src->read_buffer, frame->size) != ESP_CODEC_DEV_OK) {
        return ESP_CAPTURE_ERR_INTERNAL;
    }

    // Wake-word standby must remain extremely cheap and dependable.  The AEC
    // object stays allocated (so no connect-time allocation is needed), but
    // its DSP work is bypassed until LiveKit takes ownership of the mic.
    if (!src->processing_enabled) {
        memcpy(frame->data, src->read_buffer, frame->size);
        return ESP_CAPTURE_ERR_OK;
    }

    for (size_t i = 0; i < samples; ++i) {
        src->mic_block[src->mic_count++] = src->read_buffer[i];
        if (src->mic_count == AEC_BLOCK_SAMPLES) {
            process_aec_block(src);
            src->mic_count = 0;
        }
    }

    int16_t *output = (int16_t *)frame->data;
    for (size_t i = 0; i < samples; ++i) {
        if (src->output_count == 0) {
            // Warm-up is at most one AEC block (32 ms).  Silence here avoids
            // leaking unprocessed speaker echo before an aligned AEC result
            // becomes available.
            output[i] = 0;
        } else {
            output[i] = src->output_fifo[src->output_read];
            src->output_read = (src->output_read + 1) % AEC_OUTPUT_FIFO_SAMPLES;
            src->output_count--;
        }
    }
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_stop(esp_capture_audio_src_if_t *base)
{
    aec_audio_source_t *src = (aec_audio_source_t *)base;
    if (src->record_handle) {
        esp_codec_dev_close(src->record_handle);
    }
    src->started = false;
    clear_processing_buffers(src);
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t source_close(esp_capture_audio_src_if_t *base)
{
    (void)base;
    return ESP_CAPTURE_ERR_OK;
}

esp_capture_audio_src_if_t *aec_audio_source_create(esp_codec_dev_handle_t record_handle)
{
    if (!record_handle) {
        return NULL;
    }
    aec_audio_source_t *src = heap_caps_calloc(
        1,
        sizeof(*src),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src) {
        src = heap_caps_calloc(1, sizeof(*src), MALLOC_CAP_8BIT);
    }
    if (!src) {
        return NULL;
    }
    src->mic_block = aligned_pcm_alloc(AEC_BLOCK_SAMPLES);
    src->ref_block = aligned_pcm_alloc(AEC_BLOCK_SAMPLES);
    src->aec_block = aligned_pcm_alloc(AEC_BLOCK_SAMPLES);
    src->read_buffer = aligned_pcm_alloc(AEC_MAX_READ_SAMPLES);
    src->enhance_buf = aligned_pcm_alloc(UPLINK_ENHANCE_FRAME_SAMPLES);
    src->aec = aec_create(AEC_SAMPLE_RATE, AEC_FILTER_LENGTH, 1, AEC_MODE);
    src->ns = ns_pro_create(10, UPLINK_NS_MODE, AEC_SAMPLE_RATE);
    src->agc = esp_agc_open(UPLINK_AGC_MODE, AEC_SAMPLE_RATE);
    if (src->agc) {
        set_agc_config(
            src->agc,
            UPLINK_AGC_COMPRESSION_GAIN_DB,
            1,
            UPLINK_AGC_TARGET_DBFS);
    }
    if (!src->ns || !src->agc) {
        ESP_LOGW(TAG, "uplink NS/AGC unavailable; running AEC-only");
    }
    if (!src->mic_block || !src->ref_block || !src->aec_block || !src->read_buffer || !src->aec) {
        ESP_LOGE(TAG, "unable to allocate ESP-SR full-duplex AEC resources");
        if (src->aec) {
            aec_destroy(src->aec);
        }
        if (src->ns) {
            ns_destroy(src->ns);
        }
        if (src->agc) {
            esp_agc_close(src->agc);
        }
        free(src->mic_block);
        free(src->ref_block);
        free(src->aec_block);
        free(src->read_buffer);
        free(src->enhance_buf);
        free(src);
        return NULL;
    }
    src->base.open = source_open;
    src->base.get_support_codecs = source_get_codecs;
    src->base.set_fixed_caps = source_set_fixed_caps;
    src->base.negotiate_caps = source_negotiate_caps;
    src->base.start = source_start;
    src->base.read_frame = source_read_frame;
    src->base.stop = source_stop;
    src->base.close = source_close;
    src->record_handle = record_handle;
    src->reference_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_source = src;
    ESP_LOGI(TAG, "ESP-SR full-duplex AEC initialized: %s", aec_get_mode_string(AEC_MODE));
    return &src->base;
}

int aec_audio_source_push_playback_reference(uint8_t *data, int size, void *context)
{
    (void)context;
    aec_audio_source_t *src = s_source;
    if (!src || !src->processing_enabled || !src->far_end_active ||
        !data || size < (int)(2 * sizeof(int16_t))) {
        return 0;
    }
    // media_pipeline fixes remote output to 16 kHz, 16-bit stereo.  The two
    // slots carry the same remote speech, so the left slot is a clean mono
    // far-end reference for the ESP-SR AEC algorithm.
    const int16_t *stereo = (const int16_t *)data;
    const size_t samples = (size_t)size / (2 * sizeof(int16_t));
    size_t non_silent_samples = 0;
    taskENTER_CRITICAL(&src->reference_lock);
    for (size_t i = 0; i < samples; ++i) {
        if (src->reference_count == AEC_REFERENCE_FIFO_SAMPLES) {
            src->reference_read = (src->reference_read + 1) % AEC_REFERENCE_FIFO_SAMPLES;
            src->reference_count--;
            src->reference_overflows++;
        }
        const int16_t sample = stereo[i * 2];
        if (sample > 80 || sample < -80) {
            non_silent_samples++;
        }
        src->reference_fifo[src->reference_write] = sample;
        src->reference_write = (src->reference_write + 1) % AEC_REFERENCE_FIFO_SAMPLES;
        src->reference_count++;
    }
    src->reference_pushes++;
    src->reference_samples += samples;
    src->reference_non_silent_samples += non_silent_samples;
    if (src->reference_count > src->reference_fifo_high_water) {
        src->reference_fifo_high_water = src->reference_count;
    }
    taskEXIT_CRITICAL(&src->reference_lock);
    return 0;
}

void aec_audio_source_set_far_end_active(bool active)
{
    aec_audio_source_t *src = s_source;
    if (!src || src->far_end_active == active) {
        return;
    }
    src->far_end_active = active;
    if (active) {
        if (src->processing_enabled && !src->mic_gain_ducked) {
            src->saved_mic_gain_db = board_audio_get_mic_gain_db();
            if (board_audio_set_mic_gain_db(AEC_FAR_END_MIC_GAIN_DB) == ESP_OK) {
                src->mic_gain_ducked = true;
                ESP_LOGI(
                    TAG,
                    "far-end mic gain ducked %d -> %d dB",
                    src->saved_mic_gain_db,
                    AEC_FAR_END_MIC_GAIN_DB);
            } else {
                ESP_LOGW(TAG, "unable to duck mic gain for far-end playback");
            }
        }
    } else {
        // A far-end reply is over.  Its delayed PCM must never become the
        // reference for the next unrelated near-end utterance.
        taskENTER_CRITICAL(&src->reference_lock);
        src->reference_read = 0;
        src->reference_write = 0;
        src->reference_count = 0;
        taskEXIT_CRITICAL(&src->reference_lock);
        if (src->mic_gain_ducked) {
            const int restore_gain = src->processing_enabled
                ? AEC_ACTIVE_MIC_GAIN_DB
                : src->saved_mic_gain_db;
            src->mic_gain_ducked = false;
            if (board_audio_set_mic_gain_db(restore_gain) == ESP_OK) {
                ESP_LOGI(
                    TAG,
                    "far-end mic gain restored %d -> %d dB (active=%d)",
                    AEC_FAR_END_MIC_GAIN_DB,
                    restore_gain,
                    src->processing_enabled);
            } else {
                ESP_LOGW(TAG, "unable to restore mic gain to %d dB", restore_gain);
            }
        }
    }
}

void aec_audio_source_set_enabled(bool enabled)
{
    if (!s_source || !s_source->aec || s_source->processing_enabled == enabled) {
        return;
    }
    if (!enabled && s_source->mic_gain_ducked) {
        const int restore_gain = s_source->saved_mic_gain_db;
        s_source->mic_gain_ducked = false;
        if (board_audio_set_mic_gain_db(restore_gain) == ESP_OK) {
            ESP_LOGI(
                TAG,
                "AEC disabled; mic gain restored %d -> %d dB",
                AEC_FAR_END_MIC_GAIN_DB,
                restore_gain);
        } else {
            ESP_LOGW(TAG, "unable to restore mic gain to %d dB", restore_gain);
        }
    }
    s_source->processing_enabled = enabled;
    aec_audio_source_set_far_end_active(false);
    clear_processing_buffers(s_source);
    ESP_LOGI(TAG, "full-duplex AEC processing %s", enabled ? "enabled" : "disabled for standby");
}

void aec_audio_source_reset(void)
{
    if (s_source) {
        if (s_source->mic_gain_ducked) {
            const int restore_gain = s_source->saved_mic_gain_db;
            s_source->mic_gain_ducked = false;
            if (board_audio_set_mic_gain_db(restore_gain) == ESP_OK) {
                ESP_LOGI(
                    TAG,
                    "AEC reset; mic gain restored %d -> %d dB",
                    AEC_FAR_END_MIC_GAIN_DB,
                    restore_gain);
            } else {
                ESP_LOGW(TAG, "unable to restore mic gain to %d dB", restore_gain);
            }
        }
        clear_processing_buffers(s_source);
    }
}
