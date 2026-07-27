#pragma once

#include "esp_err.h"

typedef enum {
    BUZZER_MELODY_BOOT = 0,
    BUZZER_MELODY_WIFI_CONNECTED,
} buzzer_melody_t;

esp_err_t buzzer_init(void);
void buzzer_play_async(buzzer_melody_t melody);
