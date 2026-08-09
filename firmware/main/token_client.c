#include "token_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"

#define TOKEN_RESPONSE_LIMIT 8192

static const char *TAG = "sparkbot_token";

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || event->data_len <= 0) {
        return ESP_OK;
    }
    if (response->size + (size_t)event->data_len + 1 > TOKEN_RESPONSE_LIMIT) {
        return ESP_ERR_NO_MEM;
    }
    size_t required = response->size + (size_t)event->data_len + 1;
    if (required > response->capacity) {
        size_t new_capacity = response->capacity ? response->capacity * 2 : 1024;
        while (new_capacity < required) {
            new_capacity *= 2;
        }
        char *new_data = realloc(response->data, new_capacity);
        if (!new_data) {
            return ESP_ERR_NO_MEM;
        }
        response->data = new_data;
        response->capacity = new_capacity;
    }
    memcpy(response->data + response->size, event->data, event->data_len);
    response->size += (size_t)event->data_len;
    response->data[response->size] = '\0';
    return ESP_OK;
}

esp_err_t token_client_device_identity(char *output, size_t output_size)
{
    if (!output || output_size < TOKEN_CLIENT_ID_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "read station MAC");
    snprintf(
        output,
        output_size,
        "sparkbot-%02x%02x%02x%02x%02x%02x",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]);
    return ESP_OK;
}

static const char *json_string(cJSON *root, const char *primary, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, primary);
    if ((!item || !cJSON_IsString(item)) && fallback) {
        item = cJSON_GetObjectItemCaseSensitive(root, fallback);
    }
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool is_loopback_server_url(const char *url)
{
    if (!url) {
        return false;
    }
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    return strncmp(host, "127.0.0.1", strlen("127.0.0.1")) == 0 ||
           strncmp(host, "localhost", strlen("localhost")) == 0 ||
           strncmp(host, "[::1]", strlen("[::1]")) == 0;
}

static esp_err_t replace_loopback_server_url(livekit_credentials_t *credentials)
{
    if (!is_loopback_server_url(credentials->server_url)) {
        return ESP_OK;
    }
    if (strlen(CONFIG_SPARKBOT_FALLBACK_SERVER_URL) == 0 ||
        is_loopback_server_url(CONFIG_SPARKBOT_FALLBACK_SERVER_URL)) {
        ESP_LOGE(
            TAG,
            "token service returned loopback LiveKit URL and no LAN fallback is configured");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(
        TAG,
        "token service returned loopback URL %s; using device LAN URL %s",
        credentials->server_url,
        CONFIG_SPARKBOT_FALLBACK_SERVER_URL);
    strlcpy(
        credentials->server_url,
        CONFIG_SPARKBOT_FALLBACK_SERVER_URL,
        sizeof(credentials->server_url));
    return ESP_OK;
}

static esp_err_t parse_response(const char *body, livekit_credentials_t *credentials)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const char *server_url = json_string(root, "server_url", "url");
    const char *token = json_string(root, "token", "access_token");
    if (!server_url || !token || strlen(server_url) >= sizeof(credentials->server_url) ||
        strlen(token) >= sizeof(credentials->token)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(credentials->server_url, server_url, sizeof(credentials->server_url));
    strlcpy(credentials->token, token, sizeof(credentials->token));
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t token_client_fetch(livekit_credentials_t *credentials)
{
    if (!credentials) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(credentials, 0, sizeof(*credentials));

    if (strlen(CONFIG_SPARKBOT_TOKEN_ENDPOINT) == 0) {
        if (strlen(CONFIG_SPARKBOT_FALLBACK_SERVER_URL) == 0 ||
            strlen(CONFIG_SPARKBOT_FALLBACK_TOKEN) == 0) {
            ESP_LOGE(TAG, "no token endpoint or fallback token configured");
            return ESP_ERR_INVALID_STATE;
        }
        strlcpy(
            credentials->server_url,
            CONFIG_SPARKBOT_FALLBACK_SERVER_URL,
            sizeof(credentials->server_url));
        strlcpy(
            credentials->token,
            CONFIG_SPARKBOT_FALLBACK_TOKEN,
            sizeof(credentials->token));
        return ESP_OK;
    }

    char identity[TOKEN_CLIENT_ID_CAPACITY];
    ESP_RETURN_ON_ERROR(
        token_client_device_identity(identity, sizeof(identity)),
        TAG,
        "build device identity");

    cJSON *request = cJSON_CreateObject();
    if (!request) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request, "identity", identity);
    cJSON_AddStringToObject(request, "room", CONFIG_SPARKBOT_ROOM_NAME);
    cJSON_AddStringToObject(request, "agent_name", CONFIG_SPARKBOT_AGENT_NAME);
    char *request_body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_body) {
        return ESP_ERR_NO_MEM;
    }

    http_response_t response = { 0 };
    esp_http_client_config_t config = {
        .url = CONFIG_SPARKBOT_TOKEN_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(request_body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (strlen(CONFIG_SPARKBOT_TOKEN_API_KEY) > 0) {
        esp_http_client_set_header(client, "X-Device-Key", CONFIG_SPARKBOT_TOKEN_API_KEY);
    }
    esp_http_client_set_post_field(client, request_body, (int)strlen(request_body));

    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (result == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGE(TAG, "token endpoint returned HTTP %d", status);
        result = ESP_FAIL;
    }
    if (result == ESP_OK && (!response.data || response.size == 0)) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (result == ESP_OK) {
        result = parse_response(response.data, credentials);
    }
    if (result == ESP_OK) {
        result = replace_loopback_server_url(credentials);
    }

    esp_http_client_cleanup(client);
    free(request_body);
    free(response.data);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "received short-lived credentials for %s", identity);
    }
    return result;
}
