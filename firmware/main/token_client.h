#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOKEN_CLIENT_URL_CAPACITY   256
#define TOKEN_CLIENT_TOKEN_CAPACITY 3072
#define TOKEN_CLIENT_ID_CAPACITY    48

typedef struct {
    char server_url[TOKEN_CLIENT_URL_CAPACITY];
    char token[TOKEN_CLIENT_TOKEN_CAPACITY];
} livekit_credentials_t;

esp_err_t token_client_device_identity(char *output, size_t output_size);
esp_err_t token_client_fetch(livekit_credentials_t *credentials);

#ifdef __cplusplus
}
#endif
