#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "camera_driver.h"
#include "trigger.h"
#include "target_detector.h"
#include "scorer.h"
#include "ble_service.h"

static const char *TAG = "main";

#define PROC_TASK_STACK_SIZE  8192
#define CAMERA_TASK_STACK_SIZE 4096
#define BLE_TASK_STACK_SIZE    6144

// Shared state
static SemaphoreHandle_t s_trigger_sem;
static camera_fb_t *s_latest_fb = NULL;
static SemaphoreHandle_t s_frame_mutex;

// RGB565 to grayscale conversion
static void rgb565_to_gray(const uint8_t *rgb565, uint8_t *gray, int num_pixels)
{
    for (int i = 0; i < num_pixels; i++) {
        uint16_t pixel = rgb565[i * 2] | (rgb565[i * 2 + 1] << 8);
        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;
        gray[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
}

// Camera task: continuously capture frames, keep the latest one
static void camera_task(void *arg)
{
    ESP_LOGI(TAG, "Camera task started");

    while (1) {
        camera_fb_t *fb = camera_capture();
        if (fb) {
            xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
            if (s_latest_fb) {
                camera_return_fb(s_latest_fb);
            }
            s_latest_fb = fb;
            xSemaphoreGive(s_frame_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(30));  // ~30fps max
    }
}

// Processing task: wait for trigger, process frame, calculate score
static void proc_task(void *arg)
{
    ESP_LOGI(TAG, "Processing task started");

    while (1) {
        // Wait for trigger signal
        xSemaphoreTake(s_trigger_sem, portMAX_DELAY);

        int64_t t_start = esp_timer_get_time();
        ESP_LOGI(TAG, "=== TRIGGER PRESSED ===");
        ble_notify_status(0x01);  // detecting

        // Get latest frame
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

        // Convert RGB565 to grayscale
        uint8_t *gray = (uint8_t *)malloc(width * height);
        if (!gray) {
            ESP_LOGE(TAG, "Gray alloc failed");
            camera_return_fb(fb);
            ble_notify_status(0xFF);
            continue;
        }
        rgb565_to_gray(fb->buf, gray, width * height);
        camera_return_fb(fb);

        // Detect target
        target_result_t target = target_detect(gray, width, height);
        free(gray);

        if (!target.found) {
            ESP_LOGW(TAG, "Target not found");
            ble_notify_status(0xFF);
            continue;
        }

        // Calculate score
        target_type_t type = ble_get_target_type();
        score_result_t result = score_calculate(&target, width / 2, height / 2, type);

        int64_t t_end = esp_timer_get_time();
        float elapsed_ms = (t_end - t_start) / 1000.0f;

        const char *type_str = (type == TARGET_TYPE_PISTOL) ? "Pistol" : "Rifle";
        ESP_LOGI(TAG, "[%s] Score: %.1f | Target center: (%d, %d) | Aim: (%d, %d) | %.1f ms",
                 type_str, result.score,
                 target.center_x, target.center_y,
                 width / 2, height / 2,
                 elapsed_ms);

        // Output
        ble_notify_score(result.score);
        ble_notify_status(0x02);  // done
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DeadEyeShot starting...");

    s_trigger_sem = xSemaphoreCreateBinary();
    s_frame_mutex = xSemaphoreCreateMutex();

    // Init BLE first (so target type can be set before shooting)
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
    // BLE runs on the nimble host task (no separate task needed)

    ESP_LOGI(TAG, "DeadEyeShot ready. Waiting for trigger...");
}
