#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_TONE_WAKE_DETECTED = 0,
    BOARD_TONE_READY,
    BOARD_TONE_STANDBY,
    BOARD_TONE_ERROR,
} board_tone_t;

esp_err_t board_audio_init(void);
esp_err_t board_audio_preopen_playback(void);
esp_codec_dev_handle_t board_audio_record_handle(void);
esp_codec_dev_handle_t board_audio_playback_handle(void);
esp_err_t board_audio_set_volume(int volume);
esp_err_t board_audio_set_mic_gain_db(int gain_db);
int board_audio_get_mic_gain_db(void);
esp_err_t board_audio_play_tone(board_tone_t tone);
void board_audio_log_audio_state(const char *why);

#ifdef __cplusplus
}
#endif
