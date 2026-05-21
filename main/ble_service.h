#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "scorer.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_service_init(void);
void ble_notify_score(float score);
void ble_notify_status(uint8_t status);
target_type_t ble_get_target_type(void);

#ifdef __cplusplus
}
#endif
