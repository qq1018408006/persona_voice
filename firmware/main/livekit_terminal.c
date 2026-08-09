#include "livekit_terminal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "media_pipeline.h"

static const char *TAG = "sparkbot_livekit";
static livekit_room_handle_t s_room;
static livekit_terminal_event_callback_t s_event_callback;
static livekit_terminal_sleep_ready_callback_t s_sleep_ready_callback;
// Becomes true after the first connection attempt. The LiveKit ESP component
// (<= 0.3.10) keeps the first WebSocket Authorization header in the client
// config and silently fails to replace it on reconnect
// (esp_websocket_client_set_headers() requires a CONNECTED client), so every
// reconnect after a disconnect would send the expired first token and get
// HTTP 401 from the server. Recreate the room before those reconnects so the
// signaling transport always starts with the fresh short-lived token.
static bool s_room_used;

static void on_state_changed(livekit_connection_state_t state, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "room state: %s", livekit_connection_state_str(state));
    if ((state == LIVEKIT_CONNECTION_STATE_FAILED ||
         state == LIVEKIT_CONNECTION_STATE_DISCONNECTED) && s_room) {
        livekit_failure_reason_t reason = livekit_room_get_failure_reason(s_room);
        if (reason != LIVEKIT_FAILURE_REASON_NONE) {
            ESP_LOGE(TAG, "failure reason: %s", livekit_failure_reason_str(reason));
        }
    }
    if (s_event_callback) {
        s_event_callback(state);
    }
}

static void on_participant_info(const livekit_participant_info_t *participant, void *context)
{
    (void)context;
    if (!participant) {
        return;
    }
    ESP_LOGI(
        TAG,
        "participant identity=%s state=%d",
        participant->identity ? participant->identity : "",
        participant->state);
}

static void on_data_received(const livekit_data_received_t *data, void *context)
{
    (void)context;
    if (!data || !data->payload.bytes) {
        return;
    }
    ESP_LOGI(
        TAG,
        "data topic=%s bytes=%u",
        data->topic ? data->topic : "",
        (unsigned)data->payload.size);
    if (data->topic && data->sender_identity &&
        strncmp(data->sender_identity, "agent-", strlen("agent-")) == 0 &&
        strcmp(data->topic, "sparkbot.sleep_ready") == 0) {
        ESP_LOGI(TAG, "received orderly standby completion from voice agent");
        if (s_sleep_ready_callback) {
            s_sleep_ready_callback();
        }
    }
}

static esp_err_t livekit_terminal_create_room(void)
{
    const livekit_room_options_t options = {
        .publish = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .audio_encode = {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel_count = 1,
            },
            .capturer = media_pipeline_capture(),
        },
        .subscribe = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .renderer = media_pipeline_renderer(),
        },
        .on_state_changed = on_state_changed,
        .on_participant_info = on_participant_info,
        .on_data_received = on_data_received,
    };
    livekit_room_handle_t room = NULL;
    if (livekit_room_create(&room, &options) != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "unable to create LiveKit room");
        return ESP_FAIL;
    }
    s_room = room;
    return ESP_OK;
}

esp_err_t livekit_terminal_init(livekit_terminal_event_callback_t callback)
{
    s_event_callback = callback;
    return livekit_terminal_create_room();
}

static esp_err_t livekit_terminal_recreate_room(void)
{
    if (!s_room) {
        return livekit_terminal_create_room();
    }
    if (livekit_room_destroy(s_room) != LIVEKIT_ERR_NONE) {
        ESP_LOGW(TAG, "unable to destroy previous LiveKit room");
    }
    s_room = NULL;
    if (livekit_terminal_create_room() != ESP_OK) {
        ESP_LOGE(TAG, "unable to recreate LiveKit room");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "room recreated for a clean reconnect (fresh signaling token)");
    return ESP_OK;
}

esp_err_t livekit_terminal_connect(const livekit_credentials_t *credentials)
{
    if (!s_room || !credentials || credentials->server_url[0] == '\0' ||
        credentials->token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    livekit_connection_state_t state = livekit_room_get_state(s_room);
    if (state == LIVEKIT_CONNECTION_STATE_CONNECTED) {
        // Tolerate a wake that finds the room already connected (e.g. after
        // an unexpected reconnect): the caller has already handed the capture
        // to LiveKit, so treat this as success.
        ESP_LOGI(TAG, "connect requested while already connected; reusing room");
        return ESP_OK;
    }
    if (state != LIVEKIT_CONNECTION_STATE_DISCONNECTED &&
        state != LIVEKIT_CONNECTION_STATE_FAILED) {
        ESP_LOGW(TAG, "connect ignored in state %s", livekit_connection_state_str(state));
        return ESP_ERR_INVALID_STATE;
    }
    if (s_room_used) {
        // See s_room_used comment: a reused signaling transport would keep
        // the first (long-expired) Authorization header and the server would
        // answer 401.  A fresh room guarantees the new token is used.
        if (livekit_terminal_recreate_room() != ESP_OK) {
            return ESP_FAIL;
        }
    }
    s_room_used = true;
    return livekit_room_connect(s_room, credentials->server_url, credentials->token) ==
            LIVEKIT_ERR_NONE
        ? ESP_OK
        : ESP_FAIL;
}

esp_err_t livekit_terminal_disconnect(void)
{
    if (!s_room) {
        return ESP_ERR_INVALID_STATE;
    }
    livekit_connection_state_t state = livekit_room_get_state(s_room);
    if (state == LIVEKIT_CONNECTION_STATE_DISCONNECTED) {
        return ESP_OK;
    }
    return livekit_room_close(s_room) == LIVEKIT_ERR_NONE ? ESP_OK : ESP_FAIL;
}

livekit_connection_state_t livekit_terminal_state(void)
{
    return s_room ? livekit_room_get_state(s_room) : LIVEKIT_CONNECTION_STATE_DISCONNECTED;
}

esp_err_t livekit_terminal_send_wake_event(void)
{
    if (!s_room || livekit_room_get_state(s_room) != LIVEKIT_CONNECTION_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    static const char topic[] = "sparkbot.wake";
    static const char payload_text[] = "{\"event\":\"local_wake\",\"phrase\":\"jarvis\"}";
    livekit_data_payload_t payload = {
        .bytes = (uint8_t *)payload_text,
        .size = sizeof(payload_text) - 1,
    };
    livekit_data_publish_options_t options = {
        .payload = &payload,
        .topic = (char *)topic,
        .lossy = false,
        .destination_identities = NULL,
        .destination_identities_count = 0,
    };
    livekit_err_t result = livekit_room_publish_data(s_room, &options);
    if (result != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "publish local wake event failed: %d", result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "published local wake event for the voice agent");
    return ESP_OK;
}

esp_err_t livekit_terminal_send_sleep_request(void)
{
    if (!s_room || livekit_room_get_state(s_room) != LIVEKIT_CONNECTION_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    static const char topic[] = "sparkbot.sleep_request";
    // The actual local trigger is the same "Jarvis" wake/sleep toggle; keep
    // the datachannel payload truthful for the voice-agent side.
    static const char payload_text[] = "{\"event\":\"local_sleep_request\",\"phrase\":\"jarvis\"}";
    livekit_data_payload_t payload = {
        .bytes = (uint8_t *)payload_text,
        .size = sizeof(payload_text) - 1,
    };
    livekit_data_publish_options_t options = {
        .payload = &payload,
        .topic = (char *)topic,
        .lossy = false,
        .destination_identities = NULL,
        .destination_identities_count = 0,
    };
    livekit_err_t result = livekit_room_publish_data(s_room, &options);
    if (result != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "publish standby request failed: %d", result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "published orderly standby request for the voice agent");
    return ESP_OK;
}

esp_err_t livekit_terminal_send_sleep_cancel(void)
{
    if (!s_room || livekit_room_get_state(s_room) != LIVEKIT_CONNECTION_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    static const char topic[] = "sparkbot.sleep_cancel";
    static const char payload_text[] =
        "{\"event\":\"local_sleep_cancel\",\"reason\":\"wake_word\"}";
    livekit_data_payload_t payload = {
        .bytes = (uint8_t *)payload_text,
        .size = sizeof(payload_text) - 1,
    };
    livekit_data_publish_options_t options = {
        .payload = &payload,
        .topic = (char *)topic,
        .lossy = false,
        .destination_identities = NULL,
        .destination_identities_count = 0,
    };
    livekit_err_t result = livekit_room_publish_data(s_room, &options);
    if (result != LIVEKIT_ERR_NONE) {
        ESP_LOGE(TAG, "publish orderly standby cancellation failed: %d", result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "published orderly standby cancellation for the voice agent");
    return ESP_OK;
}

void livekit_terminal_set_sleep_ready_callback(livekit_terminal_sleep_ready_callback_t callback)
{
    s_sleep_ready_callback = callback;
}

esp_err_t livekit_terminal_set_microphone_uplink_muted(bool muted)
{
    if (!s_room) {
        return ESP_ERR_INVALID_STATE;
    }
    livekit_err_t result = livekit_room_set_audio_uplink_muted(s_room, muted);
    if (result != LIVEKIT_ERR_NONE) {
        ESP_LOGW(TAG, "cannot %s microphone uplink: %d", muted ? "mute" : "restore", result);
        return ESP_FAIL;
    }
    return ESP_OK;
}
