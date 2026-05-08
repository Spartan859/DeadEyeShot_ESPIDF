#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TRIGGER_GPIO GPIO_NUM_1

esp_err_t trigger_init(SemaphoreHandle_t trigger_sem);
