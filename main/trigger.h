#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SHOOT_GPIO GPIO_NUM_2
#define RELOAD_GPIO GPIO_NUM_1

esp_err_t trigger_init(SemaphoreHandle_t shoot_sem, SemaphoreHandle_t reload_sem);
