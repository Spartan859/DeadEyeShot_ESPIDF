#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init(void);
esp_err_t wifi_set_credentials(const char *ssid, const char *password);
bool wifi_is_connected(void);
bool wifi_has_credentials(void);
const char *wifi_get_status_text(void);
typedef void (*wifi_status_callback_t)(const char *status);
void wifi_set_status_callback(wifi_status_callback_t callback);

#ifdef __cplusplus
}
#endif
