#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "img_converters.h"
#include "camera_driver.h"
#include "trigger.h"
#include "target_detector.h"
#include "scorer.h"
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
    ESP_LOGI(TAG, "Camera task started, waiting for sensor to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        camera_fb_t *fb = camera_capture();
        if (fb) {
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

// Processing task: wait for trigger, process frame, calculate score
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

        // Save raw JPEG data for web display before processing
        uint8_t *jpeg_copy = NULL;
        int jpeg_len = fb->len;
        if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
            jpeg_copy = (uint8_t *)malloc(fb->len);
            if (jpeg_copy) {
                memcpy(jpeg_copy, fb->buf, fb->len);
            }
        }

        // Convert to grayscale for processing
        uint8_t *gray = NULL;
        if (fb->format == PIXFORMAT_RGB565) {
            gray = (uint8_t *)malloc(width * height);
            if (gray) {
                rgb565_to_gray(fb->buf, gray, width * height);
            }
        } else if (fb->format == PIXFORMAT_JPEG) {
            // JPEG → RGB888 → grayscale
            int rgb_size = width * height * 3;
            uint8_t *rgb = (uint8_t *)malloc(rgb_size);
            if (rgb && fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb)) {
                gray = (uint8_t *)malloc(width * height);
                if (gray) {
                    for (int i = 0; i < width * height; i++) {
                        gray[i] = (uint8_t)((rgb[i*3]*77 + rgb[i*3+1]*150 + rgb[i*3+2]*29) >> 8);
                    }
                }
            } else {
                ESP_LOGE(TAG, "JPEG decode failed");
            }
            free(rgb);
        }

        camera_return_fb(fb);

        if (!gray) {
            ESP_LOGE(TAG, "No grayscale data for processing");
            free(jpeg_copy);
            ble_notify_status(0xFF);
            continue;
        }

        // Detect target
        target_result_t target = target_detect(gray, width, height);
        free(gray);

        if (!target.found) {
            ESP_LOGW(TAG, "Target not found");
            // Still update web with the image even if target not found
            if (jpeg_copy) {
                web_server_update_shot(jpeg_copy, jpeg_len, width, height,
                                       0.0f, 0, 0, 0.0f, width / 2, height / 2);
                free(jpeg_copy);
            }
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

        // Update web page with shot image and score
        if (jpeg_copy) {
            web_server_update_shot(jpeg_copy, jpeg_len, width, height,
                                   result.score,
                                   target.center_x, target.center_y,
                                   target.black_radius,
                                   width / 2, height / 2);
            free(jpeg_copy);
        }

        ble_notify_score(result.score);
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
