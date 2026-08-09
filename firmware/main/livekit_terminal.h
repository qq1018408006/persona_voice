#pragma once

#include "esp_err.h"
#include "livekit.h"
#include "token_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*livekit_terminal_event_callback_t)(livekit_connection_state_t state);
typedef void (*livekit_terminal_sleep_ready_callback_t)(void);

esp_err_t livekit_terminal_init(livekit_terminal_event_callback_t callback);
esp_err_t livekit_terminal_connect(const livekit_credentials_t *credentials);
esp_err_t livekit_terminal_disconnect(void);
livekit_connection_state_t livekit_terminal_state(void);
esp_err_t livekit_terminal_send_wake_event(void);
// Requests orderly standby from the voice runtime. The microphone uplink is
// already muted by the controller when this is sent; remote audio remains on
// until the peer replies with sparkbot.sleep_ready.
esp_err_t livekit_terminal_send_sleep_request(void);
// Cancels a pending orderly-standby drain. This is emitted when the local
// wake word is detected while the device is still in the moon/draining state.
esp_err_t livekit_terminal_send_sleep_cancel(void);
void livekit_terminal_set_sleep_ready_callback(livekit_terminal_sleep_ready_callback_t callback);
// Discard microphone packets at the LiveKit RTP boundary.  Capture remains
// running so releasing this guard never sends an old queued echo burst.
esp_err_t livekit_terminal_set_microphone_uplink_muted(bool muted);

#ifdef __cplusplus
}
#endif
