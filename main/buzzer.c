#include <stdbool.h>
#include <stdint.h>

#include "buzzer.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BUZZER_GPIO GPIO_NUM_14
#define BUZZER_ACTIVE_LEVEL 1
#define BUZZER_INACTIVE_LEVEL 0
#define BUZZER_TASK_STACK_SIZE 2048
#define BUZZER_QUEUE_LENGTH 4

typedef struct {
    bool active;
    uint16_t duration_ms;
} buzzer_step_t;

static const char *TAG = "buzzer";
static QueueHandle_t s_melody_queue;

static const buzzer_step_t s_boot_pattern[] = {
    {true, 80},
    {false, 70},
    {true, 80},
    {false, 70},
    {true, 160},
};

static const buzzer_step_t s_wifi_connected_pattern[] = {
    {true, 90},
    {false, 80},
    {true, 260},
};

static void set_buzzer(bool active)
{
    gpio_set_level(BUZZER_GPIO,
                   active ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL);
}

static void play_pattern(const buzzer_step_t *steps, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        set_buzzer(steps[i].active);
        vTaskDelay(pdMS_TO_TICKS(steps[i].duration_ms));
    }
    set_buzzer(false);
}

static void buzzer_task(void *arg)
{
    buzzer_melody_t melody;
    while (1) {
        if (xQueueReceive(s_melody_queue, &melody, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (melody == BUZZER_MELODY_BOOT) {
            play_pattern(s_boot_pattern,
                         sizeof(s_boot_pattern) / sizeof(s_boot_pattern[0]));
        } else if (melody == BUZZER_MELODY_WIFI_CONNECTED) {
            play_pattern(s_wifi_connected_pattern,
                         sizeof(s_wifi_connected_pattern) /
                         sizeof(s_wifi_connected_pattern[0]));
        }
    }
}

esp_err_t buzzer_init(void)
{
    gpio_config_t buzzer_gpio_config = {
        .pin_bit_mask = 1ULL << BUZZER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&buzzer_gpio_config);
    if (err != ESP_OK) {
        return err;
    }
    set_buzzer(false);

    s_melody_queue = xQueueCreate(BUZZER_QUEUE_LENGTH, sizeof(buzzer_melody_t));
    if (!s_melody_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(buzzer_task, "buzzer", BUZZER_TASK_STACK_SIZE,
                    NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_melody_queue);
        s_melody_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Active buzzer ready on GPIO %d", BUZZER_GPIO);
    return ESP_OK;
}

void buzzer_play_async(buzzer_melody_t melody)
{
    if (!s_melody_queue) {
        return;
    }
    if (xQueueSend(s_melody_queue, &melody, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Melody queue full");
    }
}
