#include "trigger.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "trigger";
static SemaphoreHandle_t s_trigger_sem;

static void IRAM_ATTR trigger_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_trigger_sem, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t trigger_init(SemaphoreHandle_t trigger_sem)
{
    s_trigger_sem = trigger_sem;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
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

    err = gpio_isr_handler_add(TRIGGER_GPIO, trigger_isr_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ISR handler add failed: 0x%x", err);
        return err;
    }

    ESP_LOGI(TAG, "Trigger init OK (GPIO %d, falling edge)", TRIGGER_GPIO);
    return ESP_OK;
}
