#include "trigger.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "trigger";
static SemaphoreHandle_t s_shoot_sem;
static SemaphoreHandle_t s_reload_sem;
static volatile TickType_t s_last_shoot_tick;
static volatile TickType_t s_last_reload_tick;

static void IRAM_ATTR give_debounced(SemaphoreHandle_t sem, volatile TickType_t *last_tick)
{
    TickType_t now = xTaskGetTickCountFromISR();
    TickType_t debounce_ticks = pdMS_TO_TICKS(50);
    if (*last_tick != 0 && now - *last_tick < debounce_ticks) {
        return;
    }
    *last_tick = now;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR shoot_isr_handler(void *arg)
{
    give_debounced(s_shoot_sem, &s_last_shoot_tick);
}

static void IRAM_ATTR reload_isr_handler(void *arg)
{
    give_debounced(s_reload_sem, &s_last_reload_tick);
}

esp_err_t trigger_init(SemaphoreHandle_t shoot_sem, SemaphoreHandle_t reload_sem)
{
    s_shoot_sem = shoot_sem;
    s_reload_sem = reload_sem;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SHOOT_GPIO) | (1ULL << RELOAD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: 0x%x", err);
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: 0x%x", err);
        return err;
    }

    err = gpio_isr_handler_add(SHOOT_GPIO, shoot_isr_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Shoot ISR handler add failed: 0x%x", err);
        return err;
    }

    err = gpio_isr_handler_add(RELOAD_GPIO, reload_isr_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reload ISR handler add failed: 0x%x", err);
        return err;
    }

    ESP_LOGI(TAG, "Inputs ready (shoot GPIO %d, reload GPIO %d, falling edge)",
             SHOOT_GPIO, RELOAD_GPIO);
    return ESP_OK;
}
