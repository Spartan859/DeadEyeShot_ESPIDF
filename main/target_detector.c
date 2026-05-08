#include "target_detector.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "target_det";

#define FRAME_WIDTH  320
#define FRAME_HEIGHT 240
#define MAX_RING_SCANS 360

// Convert RGB565 pixel to grayscale
static inline uint8_t rgb565_to_gray(uint16_t pixel)
{
    uint8_t r = (pixel >> 11) & 0x1F;
    uint8_t g = (pixel >> 5) & 0x3F;
    uint8_t b = pixel & 0x1F;
    // Scale to 0-255 and compute luminance
    return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

// Otsu threshold: find optimal threshold to separate foreground/background
static uint8_t otsu_threshold(const uint8_t *gray, int len)
{
    int hist[256] = {0};
    for (int i = 0; i < len; i++) {
        hist[gray[i]]++;
    }

    float sum = 0;
    for (int i = 0; i < 256; i++) {
        sum += i * (float)hist[i];
    }

    float sumB = 0;
    int wB = 0;
    float maxVar = 0;
    uint8_t threshold = 0;

    for (int t = 0; t < 256; t++) {
        wB += hist[t];
        if (wB == 0) continue;
        int wF = len - wB;
        if (wF == 0) break;

        sumB += t * (float)hist[t];
        float mB = sumB / wB;
        float mF = (sum - sumB) / wF;
        float var = (float)wB * (float)wF * (mB - mF) * (mB - mF);

        if (var > maxVar) {
            maxVar = var;
            threshold = (uint8_t)t;
        }
    }
    return threshold;
}

// Simple 3x3 box blur in-place
static void blur_gray(uint8_t *gray, int w, int h)
{
    uint8_t *tmp = (uint8_t *)malloc(w * h);
    if (!tmp) return;

    memcpy(tmp, gray, w * h);

    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            int sum = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    sum += tmp[(y + dy) * w + (x + dx)];
                }
            }
            gray[y * w + x] = (uint8_t)(sum / 9);
        }
    }

    free(tmp);
}

// Find target center using axis-symmetry scanning
// For each candidate x, compute sum of |binary[left] - binary[mirror_right]| across all rows
// The x with minimum asymmetry is the target center x
static void find_symmetry_center(const uint8_t *binary, int w, int h,
                                  int *out_cx, int *out_cy)
{
    // Search column with best left-right symmetry
    int best_x = w / 2;
    int min_asym_x = INT32_MAX;

    for (int cx = 10; cx < w - 10; cx++) {
        int asym = 0;
        int max_offset = (cx < w - 1 - cx) ? cx : w - 1 - cx;
        for (int y = 0; y < h; y++) {
            for (int dx = 1; dx < max_offset; dx++) {
                int left  = binary[y * w + (cx - dx)];
                int right = binary[y * w + (cx + dx)];
                asym += (left != right);
            }
        }
        if (asym < min_asym_x) {
            min_asym_x = asym;
            best_x = cx;
        }
    }

    // Search row with best top-bottom symmetry
    int best_y = h / 2;
    int min_asym_y = INT32_MAX;

    for (int cy = 10; cy < h - 10; cy++) {
        int asym = 0;
        int max_offset = (cy < h - 1 - cy) ? cy : h - 1 - cy;
        for (int x = 0; x < w; x++) {
            for (int dy = 1; dy < max_offset; dy++) {
                int top    = binary[(cy - dy) * w + x];
                int bottom = binary[(cy + dy) * w + x];
                asym += (top != bottom);
            }
        }
        if (asym < min_asym_y) {
            min_asym_y = asym;
            best_y = cy;
        }
    }

    *out_cx = best_x;
    *out_cy = best_y;
}

// Radial scan from target center toward aim point to count ring transitions
static int count_ring_transitions(const uint8_t *binary, int w, int h,
                                   int cx, int cy, int aim_x, int aim_y,
                                   float *out_ring_width)
{
    float dx = aim_x - cx;
    float dy = aim_y - cy;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 1.0f) {
        *out_ring_width = 1.0f;
        return 0;
    }

    int steps = (int)dist;
    int transitions = 0;
    int transition_positions[20]; // track where transitions happen
    int tpos_count = 0;

    uint8_t prev = binary[cy * w + cx];
    for (int s = 1; s <= steps; s++) {
        float t = (float)s / dist;
        int px = (int)(cx + dx * t + 0.5f);
        int py = (int)(cy + dy * t + 0.5f);
        if (px < 0 || px >= w || py < 0 || py >= h) break;

        uint8_t cur = binary[py * w + px];
        if (cur != prev) {
            if (tpos_count < 20) {
                transition_positions[tpos_count++] = s;
            }
            transitions++;
            prev = cur;
        }
    }

    // Calculate average ring width from transition positions
    if (transitions >= 2) {
        float total_width = 0;
        int pairs = 0;
        for (int i = 1; i < tpos_count; i++) {
            total_width += (transition_positions[i] - transition_positions[i - 1]);
            pairs++;
        }
        *out_ring_width = (pairs > 0) ? (total_width / pairs) : 1.0f;
    } else {
        *out_ring_width = (transitions == 1) ? (float)steps : 1.0f;
    }

    return transitions;
}

target_result_t target_detect(const uint8_t *gray, int width, int height)
{
    target_result_t result = {0};

    // Work on a copy for blurring
    uint8_t *work = (uint8_t *)malloc(width * height);
    if (!work) {
        ESP_LOGE(TAG, "Alloc failed");
        return result;
    }
    memcpy(work, gray, width * height);

    // Blur
    blur_gray(work, width, height);

    // Binarize using Otsu threshold
    uint8_t thresh = otsu_threshold(work, width * height);
    uint8_t *binary = (uint8_t *)malloc(width * height);
    if (!binary) {
        free(work);
        return result;
    }

    for (int i = 0; i < width * height; i++) {
        binary[i] = (work[i] > thresh) ? 1 : 0;
    }
    free(work);

    // Find target center via symmetry
    find_symmetry_center(binary, width, height, &result.center_x, &result.center_y);

    // Radial scan from center toward image center (aim point)
    int aim_x = width / 2;
    int aim_y = height / 2;
    result.num_rings_detected = count_ring_transitions(
        binary, width, height,
        result.center_x, result.center_y,
        aim_x, aim_y,
        &result.ring_width_pixels
    );

    result.found = true;

    ESP_LOGI(TAG, "Target center: (%d, %d), ring_width: %.1f px, transitions: %d",
             result.center_x, result.center_y,
             result.ring_width_pixels, result.num_rings_detected);

    free(binary);
    return result;
}
