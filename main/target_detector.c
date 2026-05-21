#include "target_detector.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "target_det";

#define NUM_SCAN_ANGLES 72
#define LIGHT_GAP_THRESHOLD 3

target_result_t target_detect(const uint8_t *gray, int width, int height)
{
    target_result_t result = {0};
    int total = width * height;

    // 1. Adaptive threshold: for black-on-white, use mean/2
    long sum = 0;
    for (int i = 0; i < total; i++) {
        sum += gray[i];
    }
    int mean = (int)(sum / total);
    int threshold = mean / 2;
    if (threshold < 30) threshold = 30;
    if (threshold > 180) threshold = 180;

    // 2. Find centroid of dark pixels (= target center)
    long sum_x = 0, sum_y = 0;
    int dark_count = 0;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (gray[y * width + x] < threshold) {
                sum_x += x;
                sum_y += y;
                dark_count++;
            }
        }
    }

    if (dark_count < 100) {
        ESP_LOGW(TAG, "Too few dark pixels: %d", dark_count);
        return result;
    }

    int cx = (int)(sum_x / dark_count);
    int cy = (int)(sum_y / dark_count);

    // 3. Radial scan: find black circle edge at multiple angles
    float radii[NUM_SCAN_ANGLES];
    int valid_count = 0;

    for (int a = 0; a < NUM_SCAN_ANGLES; a++) {
        float angle = a * 2.0f * (float)M_PI / NUM_SCAN_ANGLES;
        float dx = cosf(angle);
        float dy = sinf(angle);

        int max_r = (width < height ? width : height) / 2;
        int last_dark_r = 0;
        int consecutive_light = 0;

        for (int r = 0; r < max_r; r++) {
            int px = cx + (int)(r * dx + 0.5f);
            int py = cy + (int)(r * dy + 0.5f);
            if (px < 0 || px >= width || py < 0 || py >= height) break;

            if (gray[py * width + px] < threshold) {
                last_dark_r = r;
                consecutive_light = 0;
            } else {
                consecutive_light++;
                if (consecutive_light >= LIGHT_GAP_THRESHOLD && last_dark_r > 0) {
                    break;
                }
            }
        }

        if (last_dark_r > 5) {
            radii[valid_count++] = (float)last_dark_r;
        }
    }

    if (valid_count < 8) {
        ESP_LOGW(TAG, "Not enough valid radii: %d", valid_count);
        return result;
    }

    // Sort and take median (robust against outliers)
    for (int i = 0; i < valid_count - 1; i++) {
        for (int j = i + 1; j < valid_count; j++) {
            if (radii[j] < radii[i]) {
                float t = radii[i];
                radii[i] = radii[j];
                radii[j] = t;
            }
        }
    }

    float radius = radii[valid_count / 2];

    result.center_x = cx;
    result.center_y = cy;
    result.black_radius = radius;
    result.found = true;

    ESP_LOGI(TAG, "Target: center=(%d,%d) radius=%.1f thresh=%d dark=%d",
             cx, cy, radius, threshold, dark_count);

    return result;
}
