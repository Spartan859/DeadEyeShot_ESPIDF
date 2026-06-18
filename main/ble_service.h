#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_service_init(void);
void ble_notify_status(uint8_t status);

#ifdef __cplusplus
}
#endif
