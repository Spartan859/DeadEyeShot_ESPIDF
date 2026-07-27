#include "buzzer.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BUZZER_GPIO GPIO_NUM_14
#define BUZZER_TIMER LEDC_TIMER_1
#define BUZZER_CHANNEL LEDC_CHANNEL_1
#define BUZZER_DUTY_RES LEDC_TIMER_10_BIT
#define BUZZER_DUTY 512
#define BUZZER_TASK_STACK_SIZE 2048
#define BUZZER_QUEUE_LENGTH 4

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
    uint16_t pause_ms;
} buzzer_note_t;

static const char *TAG = "buzzer";
static QueueHandle_t s_melody_queue;

static const buzzer_note_t s_boot_melody[] = {
    {2093, 100, 30},
    {2637, 100, 30},
    {3136, 180, 0},
};

static const buzzer_note_t s_wifi_connected_melody[] = {
    {2637, 100, 40},
    {3136, 220, 0},
};

static void silence(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
}

static void play_notes(const buzzer_note_t *notes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER,
                          notes[i].frequency_hz) == 0) {
            ESP_LOGW(TAG, "Failed to set frequency %u Hz", notes[i].frequency_hz);
            continue;
        }
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, BUZZER_DUTY);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(notes[i].duration_ms));
        silence();
        if (notes[i].pause_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(notes[i].pause_ms));
        }
    }
}

static void buzzer_task(void *arg)
{
    buzzer_melody_t melody;
    while (1) {
        if (xQueueReceive(s_melody_queue, &melody, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (melody == BUZZER_MELODY_BOOT) {
            play_notes(s_boot_melody,
                       sizeof(s_boot_melody) / sizeof(s_boot_melody[0]));
        } else if (melody == BUZZER_MELODY_WIFI_CONNECTED) {
            play_notes(s_wifi_connected_melody,
                       sizeof(s_wifi_connected_melody) /
                       sizeof(s_wifi_connected_melody[0]));
        }
    }
}

esp_err_t buzzer_init(void)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num = BUZZER_TIMER,
        .freq_hz = 2700,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel_config = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        return err;
    }

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

    ESP_LOGI(TAG, "Passive buzzer ready on GPIO %d", BUZZER_GPIO);
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
