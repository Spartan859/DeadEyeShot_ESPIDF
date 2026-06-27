#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "img_converters.h"

#include "camera_driver.h"
#include "trigger.h"
#include "target_detector.h"
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

typedef struct {
    uint8_t *rgb;
    uint8_t *gray;
    size_t rgb_size;
    size_t gray_size;
    uint32_t frames;
    uint32_t found;
    int64_t decode_total_us;
    int64_t gray_total_us;
    int64_t detect_total_us;
    int64_t total_total_us;
    int64_t decode_max_us;
    int64_t gray_max_us;
    int64_t detect_max_us;
    int64_t total_max_us;
} live_detection_benchmark_t;

static live_detection_benchmark_t s_live_det = {0};

static void clear_latest_frame(void)
{
    xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
    if (s_latest_fb) {
        camera_return_fb(s_latest_fb);
        s_latest_fb = NULL;
    }
    xSemaphoreGive(s_frame_mutex);
}

static bool ensure_live_detection_buffers(int width, int height)
{
    size_t pixels = (size_t)width * (size_t)height;
    size_t rgb_size = pixels * 3;
    size_t gray_size = pixels;
    if (s_live_det.rgb && s_live_det.gray &&
        s_live_det.rgb_size >= rgb_size && s_live_det.gray_size >= gray_size) {
        return true;
    }

    free(s_live_det.rgb);
    free(s_live_det.gray);
    s_live_det.rgb = (uint8_t *)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_live_det.gray = (uint8_t *)heap_caps_malloc(gray_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_live_det.rgb_size = s_live_det.rgb ? rgb_size : 0;
    s_live_det.gray_size = s_live_det.gray ? gray_size : 0;
    if (!s_live_det.rgb || !s_live_det.gray) {
        ESP_LOGE(TAG, "Live detection buffer allocation failed rgb=%u gray=%u",
                 (unsigned)rgb_size, (unsigned)gray_size);
        free(s_live_det.rgb);
        free(s_live_det.gray);
        s_live_det.rgb = NULL;
        s_live_det.gray = NULL;
        s_live_det.rgb_size = 0;
        s_live_det.gray_size = 0;
        return false;
    }
    ESP_LOGI(TAG, "Live detection buffers allocated rgb=%u gray=%u",
             (unsigned)rgb_size, (unsigned)gray_size);
    return true;
}

static void run_live_detection_benchmark(camera_fb_t *fb)
{
    if (!fb || fb->format != PIXFORMAT_JPEG || fb->len <= 0 || fb->width <= 0 || fb->height <= 0) {
        return;
    }
    if (!ensure_live_detection_buffers(fb->width, fb->height)) {
        return;
    }

    int64_t total_start = esp_timer_get_time();
    int64_t decode_start = total_start;
    bool decoded = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, s_live_det.rgb);
    int64_t decode_us = esp_timer_get_time() - decode_start;
    if (!decoded) {
        ESP_LOGW(TAG, "Live detection JPEG decode failed len=%u", (unsigned)fb->len);
        return;
    }

    int64_t gray_start = esp_timer_get_time();
    int pixels = fb->width * fb->height;
    for (int i = 0; i < pixels; i++) {
        int offset = i * 3;
        s_live_det.gray[i] = (uint8_t)((s_live_det.rgb[offset] + s_live_det.rgb[offset + 1] + s_live_det.rgb[offset + 2] + 1) / 3);
    }
    int64_t gray_us = esp_timer_get_time() - gray_start;

    int64_t detect_start = esp_timer_get_time();
    target_result_t target = target_detect(s_live_det.gray, fb->width, fb->height, NULL);
    int64_t detect_us = esp_timer_get_time() - detect_start;
    int64_t total_us = esp_timer_get_time() - total_start;

    s_live_det.frames++;
    if (target.found) s_live_det.found++;
    s_live_det.decode_total_us += decode_us;
    s_live_det.gray_total_us += gray_us;
    s_live_det.detect_total_us += detect_us;
    s_live_det.total_total_us += total_us;
    if (decode_us > s_live_det.decode_max_us) s_live_det.decode_max_us = decode_us;
    if (gray_us > s_live_det.gray_max_us) s_live_det.gray_max_us = gray_us;
    if (detect_us > s_live_det.detect_max_us) s_live_det.detect_max_us = detect_us;
    if (total_us > s_live_det.total_max_us) s_live_det.total_max_us = total_us;
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
                run_live_detection_benchmark(fb);
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
            if (s_live_det.frames > 0) {
                ESP_LOGI(TAG,
                         "ESP live detect %.1f fps, found %lu/%lu, decode avg/max %.1f/%.1f ms, gray avg/max %.1f/%.1f ms, detect avg/max %.1f/%.1f ms, total avg/max %.1f/%.1f ms",
                         s_live_det.frames / elapsed_s,
                         (unsigned long)s_live_det.found,
                         (unsigned long)s_live_det.frames,
                         s_live_det.decode_total_us / (float)s_live_det.frames / 1000.0f,
                         s_live_det.decode_max_us / 1000.0f,
                         s_live_det.gray_total_us / (float)s_live_det.frames / 1000.0f,
                         s_live_det.gray_max_us / 1000.0f,
                         s_live_det.detect_total_us / (float)s_live_det.frames / 1000.0f,
                         s_live_det.detect_max_us / 1000.0f,
                         s_live_det.total_total_us / (float)s_live_det.frames / 1000.0f,
                         s_live_det.total_max_us / 1000.0f);
            }
            frame_count = 0;
            log_start = now;
            capture_total_us = 0;
            capture_max_us = 0;
            video_total_us = 0;
            video_max_us = 0;
            s_live_det.frames = 0;
            s_live_det.found = 0;
            s_live_det.decode_total_us = 0;
            s_live_det.gray_total_us = 0;
            s_live_det.detect_total_us = 0;
            s_live_det.total_total_us = 0;
            s_live_det.decode_max_us = 0;
            s_live_det.gray_max_us = 0;
            s_live_det.detect_max_us = 0;
            s_live_det.total_max_us = 0;
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
