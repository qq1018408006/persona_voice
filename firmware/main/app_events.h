#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_EVENT_WAKE_WORD = 0,
    APP_EVENT_SLEEP_WORD,
    APP_EVENT_BUTTON_TOGGLE,
    APP_EVENT_BUTTON_LONG_PRESS,
    APP_EVENT_LIVEKIT_CONNECTED,
    APP_EVENT_LIVEKIT_DISCONNECTED,
    APP_EVENT_LIVEKIT_FAILED,
    APP_EVENT_REMOTE_AUDIO_STARTED,
    APP_EVENT_REMOTE_AUDIO_ENDED,
    // The voice agent has finished any pending work and playback after a
    // local standby request, so it is now safe to disconnect the room.
    APP_EVENT_SLEEP_DRAIN_COMPLETE,
} app_event_t;

typedef enum {
    APP_STATE_BOOTING = 0,
    APP_STATE_STANDBY,
    APP_STATE_CONNECTING,
    APP_STATE_ACTIVE,
    APP_STATE_SLEEP_DRAINING,
    APP_STATE_DISCONNECTING,
    APP_STATE_ERROR,
} app_state_t;

#ifdef __cplusplus
}
#endif
