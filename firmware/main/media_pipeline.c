#include "media_pipeline.h"

#include <stdbool.h>

#include "aec_audio_source.h"
#include "av_render_default.h"
#include "board_audio.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sparkbot_media";

// Full duplex route: renderer PCM is supplied to ESP-SR AEC immediately
// before the same frame is written to the ES8311/I2S output.  The microphone
// source then publishes only the cleaned near-end PCM to both the local
// WakeNet recognizer and the LiveKit uplink.
#define SPARKBOT_ENABLE_FULL_DUPLEX_AEC 1

static esp_capture_handle_t s_capture;
static esp_capture_audio_src_if_t *s_audio_source;
static esp_capture_sink_handle_t s_livekit_sink_placeholder;
static esp_capture_sink_handle_t s_speech_sink;
static audio_render_handle_t s_audio_renderer;
static av_render_handle_t s_renderer;
static bool s_local_capture_started;
static media_remote_audio_callback_t s_on_remote_audio;

// Only non-silent far-end PCM counts as a remote reply.  LiveKit keeps an
// audio transport alive between utterances, so packet arrival alone cannot be
// used for either echo control or the display state.
#define REMOTE_AUDIO_HANGOVER_MS 650
#define REMOTE_AUDIO_PEAK_MIN    220

static volatile bool s_remote_audio_seen;
static volatile TickType_t s_last_remote_audio_tick;
static TickType_t s_last_playback_level_log;
static int32_t s_playback_peak_slot0;
static int32_t s_playback_peak_slot1;

static void capture_thread_scheduler(
    const char *name,
    esp_capture_thread_schedule_cfg_t *schedule_cfg)
{
    if (!schedule_cfg) {
        return;
    }
    // ESP-SR AEC runs inside the capture thread.  Keep capture/AEC on core 1
    // so the local recognition task (pinned to core 0) is not starved.
    if (name && strcmp(name, "aenc_0") == 0) {
        // Opus encoding needs a large stack; the esp_capture example uses
        // 40 KiB for this exact task.  The default stack overflows at boot.
        schedule_cfg->stack_size = 40 * 1024;
        schedule_cfg->priority = 2;
    } else if (name && strcmp(name, "buffer_in") == 0) {
        // AEC reference ingestion can run at higher priority without
        // starving recognition, which stays on core 0.
        schedule_cfg->stack_size = 6 * 1024;
        schedule_cfg->priority = 10;
    } else if (name && strcmp(name, "AUD_SRC") == 0) {
        // ESP-SR AEC processing (aec_process) runs inside the audio source
        // task; the default stack overflows as soon as far-end playback and
        // AEC start together.
        schedule_cfg->stack_size = 24 * 1024;
    }
    schedule_cfg->core_id = 1;
}

static void remote_audio_state_update(bool speaking)
{
    aec_audio_source_set_far_end_active(speaking);
    if (s_on_remote_audio) {
        // This runs on the renderer thread.  The application callback only
        // posts a queue event and never performs display/I2S work here.
        s_on_remote_audio(speaking);
    }
}

static void remote_audio_monitor_task(void *unused)
{
    (void)unused;
    const TickType_t hangover = pdMS_TO_TICKS(REMOTE_AUDIO_HANGOVER_MS);
    while (true) {
        const bool recent = s_remote_audio_seen &&
            (xTaskGetTickCount() - s_last_remote_audio_tick) <= hangover;
        if (s_remote_audio_seen && !recent) {
            s_remote_audio_seen = false;
            remote_audio_state_update(false);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

int media_pipeline_on_playback_frame(uint8_t *data, int size, void *context)
{
    (void)context;
    // This callback is made by the I2S renderer before the PCM reaches the
    // speaker.  It drives the speaking indicator and, more importantly,
    // supplies the exact far-end PCM to ESP-SR AEC for echo cancellation.
    if (!data || size < (int)(2 * sizeof(int16_t))) {
        return aec_audio_source_push_playback_reference(data, size, context);
    }

    const int16_t *pcm = (const int16_t *)data;
    const size_t sample_count = (size_t)size / sizeof(int16_t);
    int32_t peak = 0;
    // Output is fixed to stereo.  Track both slots so a silent right/left
    // channel can be distinguished from a truly silent ES8311 output.
    for (size_t i = 0; i < sample_count; i += 2) {
        const int32_t value0 = pcm[i];
        const int32_t value1 = pcm[i + 1];
        const int32_t magnitude0 = value0 < 0 ? -value0 : value0;
        const int32_t magnitude1 = value1 < 0 ? -value1 : value1;
        if (magnitude0 > peak) {
            peak = magnitude0;
        }
        if (magnitude0 > s_playback_peak_slot0) {
            s_playback_peak_slot0 = magnitude0;
        }
        if (magnitude1 > s_playback_peak_slot1) {
            s_playback_peak_slot1 = magnitude1;
        }
    }
    const TickType_t now = xTaskGetTickCount();
    if (s_last_playback_level_log == 0) {
        s_last_playback_level_log = now;
    } else if (now - s_last_playback_level_log >= pdMS_TO_TICKS(2000)) {
        ESP_LOGI(
            TAG,
            "remote playback PCM peak slot0=%" PRIu32 " slot1=%" PRIu32,
            (uint32_t)s_playback_peak_slot0,
            (uint32_t)s_playback_peak_slot1);
        s_last_playback_level_log = now;
        s_playback_peak_slot0 = 0;
        s_playback_peak_slot1 = 0;
    }
    if (peak >= REMOTE_AUDIO_PEAK_MIN) {
        const bool was_recent = s_remote_audio_seen &&
            (now - s_last_remote_audio_tick) <= pdMS_TO_TICKS(REMOTE_AUDIO_HANGOVER_MS);
        s_last_remote_audio_tick = now;
        s_remote_audio_seen = true;
        if (!was_recent) {
            remote_audio_state_update(true);
        }
    }
    // Mark the beginning of a far-end reply before pushing this exact render
    // frame, so the AEC diagnostics include the first audible block.
    return aec_audio_source_push_playback_reference(data, size, context);
}

esp_err_t media_pipeline_init(void)
{
    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    esp_codec_dev_handle_t record = board_audio_record_handle();
    ESP_RETURN_ON_FALSE(record, ESP_ERR_INVALID_STATE, TAG, "record codec is unavailable");
    esp_capture_audio_dev_src_cfg_t source_config = {
        .record_handle = record,
    };
#if SPARKBOT_ENABLE_FULL_DUPLEX_AEC
    s_audio_source = aec_audio_source_create(record);
    if (s_audio_source) {
        ESP_LOGI(TAG, "using ESP-SR full-duplex AEC microphone source");
    }
#endif
    if (!s_audio_source) {
        // Fail safely only if the AEC object cannot be allocated.  This
        // should not occur on the SparkBot S3 with PSRAM.
        s_audio_source = esp_capture_new_audio_dev_src(&source_config);
        ESP_LOGW(TAG, "AEC unavailable; using raw microphone fallback");
    }
    ESP_LOGI(TAG, "microphone source ready; stop phrases still control interruption");
    ESP_RETURN_ON_FALSE(s_audio_source, ESP_FAIL, TAG, "create microphone source");

    esp_capture_cfg_t capture_config = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = s_audio_source,
    };
    ESP_RETURN_ON_FALSE(
        esp_capture_set_thread_scheduler(capture_thread_scheduler) == ESP_CAPTURE_ERR_OK,
        ESP_FAIL,
        TAG,
        "set capture thread scheduler");
    ESP_RETURN_ON_FALSE(
        esp_capture_open(&capture_config, &s_capture) == ESP_CAPTURE_ERR_OK,
        ESP_FAIL,
        TAG,
        "open capture pipeline");

    // esp_capture path 0 is the primary pipeline.  The speech-recognition
    // path below is path 1 and shares its source, so path 0 must exist first
    // even while it is disabled in standby.  LiveKit later asks for this
    // exact Opus configuration and reuses the existing path 0 handle.
    esp_capture_sink_cfg_t livekit_sink_config = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_OPUS,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    ESP_RETURN_ON_FALSE(
        esp_capture_sink_setup(
            s_capture,
            0,
            &livekit_sink_config,
            &s_livekit_sink_placeholder) == ESP_CAPTURE_ERR_OK,
        ESP_FAIL,
        TAG,
        "create disabled LiveKit primary sink");

    esp_capture_sink_cfg_t speech_sink_config = {
        .audio_info = {
            .format_id = ESP_CAPTURE_FMT_ID_PCM,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    ESP_RETURN_ON_FALSE(
        esp_capture_sink_setup(s_capture, 1, &speech_sink_config, &s_speech_sink) ==
            ESP_CAPTURE_ERR_OK,
        ESP_FAIL,
        TAG,
        "create local speech-recognition sink");
    ESP_RETURN_ON_FALSE(
        esp_capture_sink_enable(s_speech_sink, ESP_CAPTURE_RUN_MODE_ALWAYS) ==
            ESP_CAPTURE_ERR_OK,
        ESP_FAIL,
        TAG,
        "enable local speech-recognition sink");

    esp_codec_dev_handle_t playback = board_audio_playback_handle();
    ESP_RETURN_ON_FALSE(playback, ESP_ERR_INVALID_STATE, TAG, "playback codec is unavailable");
    i2s_render_cfg_t audio_render_config = {
        .play_handle = playback,
        .cb = media_pipeline_on_playback_frame,
        .fixed_clock = true,
    };
    s_audio_renderer = av_render_alloc_i2s_render(&audio_render_config);
    ESP_RETURN_ON_FALSE(s_audio_renderer, ESP_FAIL, TAG, "create I2S renderer");

    av_render_cfg_t renderer_config = {
        .audio_render = s_audio_renderer,
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 96 * 1024,
        .allow_drop_data = false,
    };
    s_renderer = av_render_open(&renderer_config);
    ESP_RETURN_ON_FALSE(s_renderer, ESP_FAIL, TAG, "open audio renderer");
    av_render_audio_frame_info_t output_format = {
        .sample_rate = 16000,
        // SparkBot's ES8311 is wired to a two-slot I2S bus.  The remote
        // Gemini stream is usually mono, but av_render's resampler can copy
        // it to both slots.  Keeping the output stereo also avoids changing
        // the shared I2S slot layout while the microphone is active.
        .channel = 2,
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(s_renderer, &output_format);
    ESP_RETURN_ON_FALSE(
        xTaskCreate(remote_audio_monitor_task, "sparkbot_remote_audio", 4096, NULL, 4, NULL) == pdPASS,
        ESP_ERR_NO_MEM,
        TAG,
        "start remote audio monitor");
    ESP_LOGI(
        TAG,
        "audio pipeline ready: disabled LiveKit/Opus primary sink 0, standby PCM sink 1");
    return ESP_OK;
}

esp_capture_handle_t media_pipeline_capture(void)
{
    return s_capture;
}

av_render_handle_t media_pipeline_renderer(void)
{
    return s_renderer;
}

esp_capture_sink_handle_t media_pipeline_speech_sink(void)
{
    return s_speech_sink;
}

void media_pipeline_set_remote_audio_callback(media_remote_audio_callback_t callback)
{
    s_on_remote_audio = callback;
}

esp_err_t media_pipeline_reset_remote_audio(void)
{
    if (!s_renderer) {
        return ESP_ERR_INVALID_STATE;
    }

    // The renderer persists across rooms, but its decoder/render tasks and
    // playback device belong to one remote-audio session.  Resetting after a
    // completed disconnect prevents a delayed remote frame from racing the
    // next standby microphone start on the shared I2S peripheral.
    int result = av_render_reset(s_renderer);
    if (result != ESP_MEDIA_ERR_OK) {
        ESP_LOGE(TAG, "unable to reset remote-audio renderer: %d", result);
        return ESP_FAIL;
    }
    aec_audio_source_reset();
    aec_audio_source_set_far_end_active(false);
    ESP_LOGI(TAG, "remote-audio renderer reset for standby");
    return ESP_OK;
}

esp_err_t media_pipeline_pause_remote_audio(void)
{
    if (!s_renderer) {
        return ESP_ERR_INVALID_STATE;
    }

    s_remote_audio_seen = false;
    ESP_LOGI(TAG, "pause remote audio: clearing AEC far-end state");
    aec_audio_source_set_far_end_active(false);
    ESP_LOGI(TAG, "pause remote audio: disabling AEC processing");
    aec_audio_source_set_enabled(false);
    ESP_LOGI(TAG, "pause remote audio: resetting AEC buffers");
    aec_audio_source_reset();
    // LiveKit stops the shared microphone/I2S pipeline on disconnect. Pause
    // rendering first so a queued decoded frame cannot write after that I2S
    // output has been released.
    ESP_LOGI(TAG, "pause remote audio: pausing renderer");
    const int result = av_render_pause(s_renderer, true);
    if (result != ESP_MEDIA_ERR_OK) {
        ESP_LOGW(TAG, "unable to pause remote audio renderer: %d", result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "pause remote audio: renderer paused");
    return ESP_OK;
}

esp_err_t media_pipeline_resume_remote_audio(void)
{
    if (!s_renderer) {
        return ESP_ERR_INVALID_STATE;
    }

    s_remote_audio_seen = false;
    aec_audio_source_set_far_end_active(false);
    aec_audio_source_reset();
    aec_audio_source_set_enabled(true);
    const int result = av_render_pause(s_renderer, false);
    if (result != ESP_MEDIA_ERR_OK) {
        ESP_LOGW(TAG, "unable to resume remote audio renderer: %d", result);
        return ESP_FAIL;
    }
    // The ES8311 codec is shared with the always-on capture path.  Re-assert
    // output volume and unmute after a warm-standby pause/resume cycle so a
    // codec state reset cannot leave Gemini audio silently written to I2S.
    const esp_err_t volume_result =
        board_audio_set_volume(CONFIG_SPARKBOT_SPEAKER_VOLUME);
    if (volume_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "re-assert speaker volume %d failed: %s",
            CONFIG_SPARKBOT_SPEAKER_VOLUME,
            esp_err_to_name(volume_result));
    }
    const int mute_result =
        esp_codec_dev_set_out_mute(board_audio_playback_handle(), false);
    if (mute_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(
            TAG,
            "un-mute speaker after resume failed: %d",
            mute_result);
    }
    int dac_volume_reg = 0;
    int dac_control_reg = 0;
    const int read_volume =
        esp_codec_dev_read_reg(board_audio_playback_handle(), 0x32, &dac_volume_reg);
    const int read_control =
        esp_codec_dev_read_reg(board_audio_playback_handle(), 0x31, &dac_control_reg);
    ESP_LOGI(
        TAG,
        "ES8311 DAC reg31(ctrl)=0x%02X reg32(vol)=0x%02X (read=%d/%d)",
        dac_control_reg,
        dac_volume_reg,
        read_control,
        read_volume);
    board_audio_log_audio_state("at resume");
    ESP_LOGI(TAG, "remote-audio renderer resumed from warm standby");
    return ESP_OK;
}

esp_err_t media_pipeline_start_local(void)
{
    s_remote_audio_seen = false;
    aec_audio_source_set_far_end_active(false);
    aec_audio_source_set_enabled(false);
    // The LiveKit room owns the shared capture while connected.  Its teardown
    // can stop the pipeline (and even disable sinks) without clearing our
    // local flag, so a plain early return would leave WakeNet deaf forever.
    // Always force a stop first, then re-enable the local PCM sink and start.
    if (s_local_capture_started) {
        esp_capture_stop(s_capture);
        s_local_capture_started = false;
    }
    // LiveKit enables path 0 while connected.  Its engine stops capture on
    // disconnect but leaves that path enabled.  Disable it before standby so
    // an unconsumed Opus queue cannot back-pressure the always-on PCM path.
    if (esp_capture_sink_enable(
            s_livekit_sink_placeholder,
            ESP_CAPTURE_RUN_MODE_DISABLE) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "unable to disable LiveKit sink for standby");
        return ESP_FAIL;
    }
    // The local speech-recognition sink must be explicitly (re)enabled: the
    // LiveKit SDK teardown can leave it disabled, which makes the standby
    // PCM feed return "acquire failed" forever.
    if (esp_capture_sink_enable(
            s_speech_sink,
            ESP_CAPTURE_RUN_MODE_ALWAYS) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "unable to enable standby speech sink");
        return ESP_FAIL;
    }
    // Re-assert the playback endpoint before the local capture opens the
    // shared I2S interface.  If av_render closed it during the previous
    // session reset, this open configures TX as stereo 16 kHz again while
    // nothing else is running, so the next wake cannot fail on 0x103.
    if (board_audio_preopen_playback() != ESP_OK) {
        ESP_LOGE(TAG, "unable to pre-open playback endpoint for standby");
        return ESP_FAIL;
    }
    if (esp_capture_start(s_capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "unable to start standby microphone capture");
        return ESP_FAIL;
    }
    s_local_capture_started = true;
    ESP_LOGI(TAG, "standby PCM capture started; LiveKit remains disconnected");
    return ESP_OK;
}

esp_err_t media_pipeline_stop_local(void)
{
    if (!s_local_capture_started) {
        return ESP_OK;
    }
    if (esp_capture_stop(s_capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "unable to stop standby microphone capture");
        return ESP_FAIL;
    }
    s_local_capture_started = false;
    return ESP_OK;
}

esp_err_t media_pipeline_prepare_livekit(void)
{
    if (!s_livekit_sink_placeholder) {
        return ESP_ERR_INVALID_STATE;
    }

    s_remote_audio_seen = false;
    aec_audio_source_set_far_end_active(false);
    aec_audio_source_set_enabled(true);
    // livekit_room_create() enables this sink only once, at boot.  Because
    // standby later disables it to protect the local WakeNet PCM path, it
    // must be enabled again before LiveKit calls esp_capture_start() for the
    // next room.  Without this step the microphone is live for WakeNet but
    // path 0 produces no Opus frames for RTP.
    if (esp_capture_sink_enable(
            s_livekit_sink_placeholder,
            ESP_CAPTURE_RUN_MODE_ALWAYS) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "unable to enable LiveKit Opus sink");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LiveKit Opus capture sink re-enabled");
    return ESP_OK;
}
