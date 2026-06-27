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

static void clear_latest_frame(void)
{
    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    if (s_latest_fb) {
        camera_return_fb(s_latest_fb);
        s_latest_fb = NULL;
    }
    xSemaphoreGive(s_frame_mutex);
}

#if CONFIG_DEADEYE_CAMERA_FPS_TEST
static void camera_fps_test(void)
{
    ESP_LOGI(TAG, "Camera FPS test mode");

    uint32_t frame_count = 0;
    int64_t log_start = esp_timer_get_time();
    int64_t capture_total_us = 0;
    int64_t capture_max_us = 0;

    while (1) {
        int64_t capture_start = esp_timer_get_time();
        camera_fb_t *fb = camera_capture();
        int64_t capture_us = esp_timer_get_time() - capture_start;

        if (!fb) {
            ESP_LOGW(TAG, "Camera FPS test capture failed");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        frame_count++;
        capture_total_us += capture_us;
        if (capture_us > capture_max_us) {
            capture_max_us = capture_us;
        }

        camera_return_fb(fb);

        int64_t now = esp_timer_get_time();
        if (now - log_start >= 5000000 && frame_count > 0) {
            float elapsed_s = (now - log_start) / 1000000.0f;
            ESP_LOGI(TAG,
                     "Camera FPS test %.1f fps, capture avg/max %.1f/%.1f ms",
                     frame_count / elapsed_s,
                     capture_total_us / (float)frame_count / 1000.0f,
                     capture_max_us / 1000.0f);
            frame_count = 0;
            log_start = now;
            capture_total_us = 0;
            capture_max_us = 0;
        }
    }
}
#endif

// Camera task: continuously capture frames, keep the latest one
static void camera_task(void *arg)
{
    ESP_LOGI(TAG, "Camera task started, waiting for sensor to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint32_t frame_count = 0;
    int64_t log_start = esp_timer_get_time();
    int64_t capture_total_us = 0;
    int64_t capture_max_us = 0;
    int64_t video_total_us = 0;
    int64_t video_max_us = 0;

    while (1) {
        if (!camera_wants_active()) {
            clear_latest_frame();
            if (camera_is_active()) {
                camera_deinit();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!camera_is_active() && camera_init() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int64_t capture_start = esp_timer_get_time();
        camera_fb_t *fb = camera_capture();
        int64_t capture_us = esp_timer_get_time() - capture_start;
        if (fb) {
            if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
                int64_t video_start = esp_timer_get_time();
                web_server_update_video_frame(fb->buf, fb->len, fb->width, fb->height);
                int64_t video_us = esp_timer_get_time() - video_start;
                video_total_us += video_us;
                if (video_us > video_max_us) {
                    video_max_us = video_us;
                }
            }
            xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
            if (s_latest_fb) {
                camera_return_fb(s_latest_fb);
            }
            s_latest_fb = fb;
            xSemaphoreGive(s_frame_mutex);
            frame_count++;
            capture_total_us += capture_us;
            if (capture_us > capture_max_us) {
                capture_max_us = capture_us;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (now - log_start >= 5000000 && frame_count > 0) {
            float elapsed_s = (now - log_start) / 1000000.0f;
            ESP_LOGI(TAG,
                     "Camera %.1f fps, capture avg/max %.1f/%.1f ms, video copy avg/max %.1f/%.1f ms",
                     frame_count / elapsed_s,
                     capture_total_us / (float)frame_count / 1000.0f,
                     capture_max_us / 1000.0f,
                     video_total_us / (float)frame_count / 1000.0f,
                     video_max_us / 1000.0f);
            frame_count = 0;
            log_start = now;
            capture_total_us = 0;
            capture_max_us = 0;
            video_total_us = 0;
            video_max_us = 0;
        }
        taskYIELD();
    }
}

// Processing task: wait for trigger and publish the latest JPEG frame
static void proc_task(void *arg)
{
    ESP_LOGI(TAG, "Processing task started");

    while (1) {
        xSemaphoreTake(s_trigger_sem, portMAX_DELAY);

        if (!camera_wants_active() || !camera_is_active()) {
            ESP_LOGW(TAG, "Trigger ignored while camera is inactive");
            continue;
        }

        int64_t t_start = esp_timer_get_time();
        ESP_LOGI(TAG, "=== TRIGGER PRESSED ===");
        int width = 0;
        int height = 0;
        int jpeg_len = 0;
        uint8_t *jpeg_copy = NULL;

        xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
        camera_fb_t *fb = s_latest_fb;
        s_latest_fb = NULL;
        if (fb) {
            width = fb->width;
            height = fb->height;
            jpeg_len = fb->len;
            if (fb->format == PIXFORMAT_JPEG && fb->len > 0) {
                jpeg_copy = (uint8_t *)malloc(fb->len);
                if (jpeg_copy) {
                    memcpy(jpeg_copy, fb->buf, fb->len);
                }
            } else {
                ESP_LOGE(TAG, "Frame is not JPEG: format=%d len=%d", fb->format, fb->len);
            }
            camera_return_fb(fb);
        }
        xSemaphoreGive(s_frame_mutex);

        if (!jpeg_copy) {
            ESP_LOGW(TAG, "No JPEG frame available");
            continue;
        }

        int64_t t_end = esp_timer_get_time();
        float elapsed_ms = (t_end - t_start) / 1000.0f;

        web_server_update_shot(jpeg_copy, jpeg_len, width, height);
        ESP_LOGI(TAG, "Shot captured: %dx%d, %d bytes, %.1f ms",
                 width, height, jpeg_len, elapsed_ms);
        free(jpeg_copy);

    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DeadEyeShot starting...");

#if CONFIG_DEADEYE_CAMERA_FPS_TEST
    {
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);
    }

    camera_request_active(true);
    ESP_ERROR_CHECK(camera_init());
    camera_fps_test();
    return;
#endif

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

    // Init BLE provisioning
    ESP_ERROR_CHECK(ble_service_init());

    camera_request_active(false);

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
