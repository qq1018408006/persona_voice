#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_UI_BOOTING = 0,
    BOARD_UI_STANDBY,
    BOARD_UI_CONNECTING,
    BOARD_UI_LISTENING,
    BOARD_UI_PROCESSING,
    // A local standby request is pending: microphone uplink is closed but
    // remote speech/background work is still being drained before disconnect.
    BOARD_UI_SLEEP_DRAINING,
    BOARD_UI_SPEAKING,
    // Warm standby is fully connected and ready: the user may wake now.
    BOARD_UI_READY,
    BOARD_UI_ERROR,
} board_ui_state_t;

typedef void (*board_button_callback_t)(void);
#define BOARD_BUTTON_LONG_PRESS_MS 1000

esp_err_t board_ui_init(
    board_button_callback_t button_callback);
// Optional second button callback fired once when BOOT is held for
// BOARD_BUTTON_LONG_PRESS_MS.  Used for a local tone self-test.
void board_ui_set_long_press_callback(board_button_callback_t callback);
void board_ui_set_state(board_ui_state_t state);
void board_ui_show_volume(int level, int level_count);
void board_ui_show_sensitivity(int level, int level_count);
void board_ui_pulse(void);

#ifdef __cplusplus
}
#endif
