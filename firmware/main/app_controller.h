#pragma once

#include <stdbool.h>

#include "app_events.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_controller_start(void);
bool app_controller_post(app_event_t event);
app_state_t app_controller_get_state(void);

#ifdef __cplusplus
}
#endif
