#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "camera_driver.h"
#include "trigger.h"
#include "ble_service.h"
#include "wifi_station.h"
#include "web_server.h"

static const char *TAG = "main";

#define PROC_TASK_STACK_SIZE  8192
#define CAMERA_TASK_STACK_SIZE 4096

// Shared state
static SemaphoreHandle_t s_trigger_sem;
static camera_fb_t *s_latest_fb = NULL;
static SemaphoreHandle_t s_frame_mutex;

// Camera task: continuously capture frames, keep the latest one
static void camera_task(void *arg)
{
    ESP_LOGI(TAG, "Camera task started, waiting for sensor to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        camera_fb_t *fb = camera_capture();
        if (fb) {
            if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
                web_server_update_video_frame(fb->buf, fb->len, fb->width, fb->height);
            }
            xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
            if (s_latest_fb) {
                camera_return_fb(s_latest_fb);
            }
            s_latest_fb = fb;
            xSemaphoreGive(s_frame_mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Processing task: wait for trigger and publish the latest JPEG frame
static void proc_task(void *arg)
{
    ESP_LOGI(TAG, "Processing task started");

    while (1) {
        xSemaphoreTake(s_trigger_sem, portMAX_DELAY);

        int64_t t_start = esp_timer_get_time();
        ESP_LOGI(TAG, "=== TRIGGER PRESSED ===");
        ble_notify_status(0x01);

        camera_fb_t *fb = NULL;
        xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
        fb = s_latest_fb;
        s_latest_fb = NULL;
        xSemaphoreGive(s_frame_mutex);

        if (!fb) {
            ESP_LOGW(TAG, "No frame available");
            ble_notify_status(0xFF);
            continue;
        }

        int width = fb->width;
        int height = fb->height;
        int jpeg_len = fb->len;

        uint8_t *jpeg_copy = NULL;
        if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
            jpeg_copy = (uint8_t *)malloc(fb->len);
            if (jpeg_copy) {
                memcpy(jpeg_copy, fb->buf, fb->len);
            }
        } else {
            ESP_LOGE(TAG, "Frame is not JPEG: format=%d len=%d", fb->format, fb->len);
        }

        camera_return_fb(fb);

        if (!jpeg_copy) {
            ESP_LOGE(TAG, "No JPEG data for upload");
            ble_notify_status(0xFF);
            continue;
        }

        int64_t t_end = esp_timer_get_time();
        float elapsed_ms = (t_end - t_start) / 1000.0f;

        web_server_update_shot(jpeg_copy, jpeg_len, width, height);
        ESP_LOGI(TAG, "Shot captured: %dx%d, %d bytes, %.1f ms",
                 width, height, jpeg_len, elapsed_ms);
        free(jpeg_copy);

        ble_notify_status(0x02);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DeadEyeShot starting...");

    s_trigger_sem = xSemaphoreCreateBinary();
    s_frame_mutex = xSemaphoreCreateMutex();

    // Init NVS (required by BLE and WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Init WiFi
    ESP_ERROR_CHECK(wifi_init());

    // Init web server
    ESP_ERROR_CHECK(web_server_init());

    // Init BLE
    ESP_ERROR_CHECK(ble_service_init());

    // Init camera
    ESP_ERROR_CHECK(camera_init());

    // Init trigger GPIO
    ESP_ERROR_CHECK(trigger_init(s_trigger_sem));

    // Create tasks
    xTaskCreatePinnedToCore(camera_task, "camera", CAMERA_TASK_STACK_SIZE,
                            NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(proc_task, "proc", PROC_TASK_STACK_SIZE,
                            NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "DeadEyeShot ready. Waiting for trigger...");
    ESP_LOGI(TAG, "Web UI: http://<IP>:%d/", WEB_SERVER_PORT);
}
