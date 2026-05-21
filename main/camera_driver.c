#include "camera_driver.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG = "camera";

// Pin definitions from Sketch_07.1_CameraWebServer (ESP32S3_EYE)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y2_GPIO_NUM       11
#define Y3_GPIO_NUM       9
#define Y4_GPIO_NUM       8
#define Y5_GPIO_NUM       10
#define Y6_GPIO_NUM       12
#define Y7_GPIO_NUM       18
#define Y8_GPIO_NUM       17
#define Y9_GPIO_NUM       16

#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

esp_err_t camera_init(void)
{
    bool psram_available = (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
    ESP_LOGI(TAG, "PSRAM %s (%lu bytes)",
             psram_available ? "detected" : "NOT detected",
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Free heap: %lu bytes, free internal: %lu bytes",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    camera_config_t config = {
        .pin_pwdn     = PWDN_GPIO_NUM,
        .pin_reset    = RESET_GPIO_NUM,
        .pin_xclk     = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7       = Y9_GPIO_NUM,
        .pin_d6       = Y8_GPIO_NUM,
        .pin_d5       = Y7_GPIO_NUM,
        .pin_d4       = Y6_GPIO_NUM,
        .pin_d3       = Y5_GPIO_NUM,
        .pin_d2       = Y4_GPIO_NUM,
        .pin_d1       = Y3_GPIO_NUM,
        .pin_d0       = Y2_GPIO_NUM,
        .pin_vsync    = VSYNC_GPIO_NUM,
        .pin_href     = HREF_GPIO_NUM,
        .pin_pclk     = PCLK_GPIO_NUM,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        // Use JPEG first to diagnose if camera works at all
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    if (psram_available) {
        config.fb_count    = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode   = CAMERA_GRAB_LATEST;
        ESP_LOGI(TAG, "Using PSRAM frame buffers (x2)");
    } else {
        config.fb_count    = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        ESP_LOGI(TAG, "PSRAM not available, using DRAM frame buffer (x1)");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        if (psram_available) {
            ESP_LOGW(TAG, "PSRAM alloc failed, retrying with DRAM");
            esp_camera_deinit();
            config.fb_count    = 1;
            config.fb_location = CAMERA_FB_IN_DRAM;
            err = esp_camera_init(&config);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
            return err;
        }
    }

    sensor_t *s = esp_camera_sensor_get();
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, 0);

    ESP_LOGI(TAG, "Camera init OK (QVGA JPEG, %s)",
             config.fb_location == CAMERA_FB_IN_PSRAM ? "PSRAM" : "DRAM");

    // Test capture to verify camera works
    ESP_LOGI(TAG, "Test capture...");
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        ESP_LOGI(TAG, "Test capture OK: %dx%d, %d bytes, format=%d",
                 fb->width, fb->height, fb->len, fb->format);
        esp_camera_fb_return(fb);
    } else {
        ESP_LOGE(TAG, "Test capture FAILED");
    }

    return ESP_OK;
}

camera_fb_t *camera_capture(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
    }
    return fb;
}

void camera_return_fb(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}
