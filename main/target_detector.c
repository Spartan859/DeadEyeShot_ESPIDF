#include "target_detector.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "target_det";

#define MAX_LABELS 8192
#define MIN_COMPONENT_AREA 200
#define MIN_CIRCULARITY 0.8f

typedef struct {
    int *parent;
    int8_t *rnk;
} uf_ctx_t;

static int uf_find(uf_ctx_t *uf, int x)
{
    while (uf->parent[x] != x) {
        uf->parent[x] = uf->parent[uf->parent[x]];
        x = uf->parent[x];
    }
    return x;
}

static void uf_union(uf_ctx_t *uf, int a, int b)
{
    a = uf_find(uf, a);
    b = uf_find(uf, b);
    if (a == b) return;
    if (uf->rnk[a] < uf->rnk[b]) { int t = a; a = b; b = t; }
    uf->parent[b] = a;
    if (uf->rnk[a] == uf->rnk[b]) uf->rnk[a]++;
}

target_result_t target_detect(const uint8_t *gray, int width, int height,
                               uint8_t *mask_out)
{
    target_result_t result = {0};
    int total = width * height;

    long sum = 0;
    for (int i = 0; i < total; i++) sum += gray[i];
    int mean = (int)(sum / total);

    int threshold = mean / 2;
    if (threshold < 30) threshold = 30;
    if (threshold > 180) threshold = 180;

    int step = (int)(mean * 0.05f);
    if (step < 5) step = 5;
    int thresh_max = (int)(mean * 0.9f);

    // Allocate all buffers once (reused across retries)
    uint8_t *bin = (uint8_t *)malloc(total);
    uint16_t *labels = (uint16_t *)malloc(total * sizeof(uint16_t));
    int *uf_parent = (int *)malloc(MAX_LABELS * sizeof(int));
    int8_t *uf_rnk = (int8_t *)calloc(MAX_LABELS, sizeof(int8_t));
    int *comp_area  = (int *)malloc(MAX_LABELS * sizeof(int));
    int *comp_sx    = (int *)malloc(MAX_LABELS * sizeof(int));
    int *comp_sy    = (int *)malloc(MAX_LABELS * sizeof(int));
    int *comp_perim = (int *)malloc(MAX_LABELS * sizeof(int));

    if (!bin || !labels || !uf_parent || !uf_rnk ||
        !comp_area || !comp_sx || !comp_sy || !comp_perim) {
        ESP_LOGE(TAG, "OOM in target_detect");
        goto cleanup;
    }

    for (int tries = 0; ; tries++) {
        // Build binary mask
        int dark_count = 0;
        for (int i = 0; i < total; i++) {
            bin[i] = (gray[i] < threshold) ? 1 : 0;
            dark_count += bin[i];
        }
        float dark_ratio = (float)dark_count / total;

        // Update mask output (last attempt wins)
        if (mask_out) {
            for (int i = 0; i < total; i++)
                mask_out[i] = bin[i] ? 255 : 0;
        }

        // CC labeling - pass 1
        memset(labels, 0, total * sizeof(uint16_t));
        for (int i = 0; i < MAX_LABELS; i++) uf_parent[i] = i;
        memset(uf_rnk, 0, MAX_LABELS);

        uf_ctx_t uf = { uf_parent, uf_rnk };
        int next_label = 1;
        bool overflow = false;

        for (int y = 0; y < height && !overflow; y++) {
            for (int x = 0; x < width; x++) {
                int idx = y * width + x;
                if (!bin[idx]) continue;

                uint16_t left = (x > 0) ? labels[idx - 1] : 0;
                uint16_t top  = (y > 0) ? labels[idx - width] : 0;

                if (left == 0 && top == 0) {
                    if (next_label >= MAX_LABELS) { overflow = true; break; }
                    labels[idx] = next_label++;
                } else if (left != 0 && top == 0) {
                    labels[idx] = left;
                } else if (left == 0 && top != 0) {
                    labels[idx] = top;
                } else {
                    labels[idx] = (left < top) ? left : top;
                    if (left != top) uf_union(&uf, left, top);
                }
            }
        }

        // Pass 2: resolve
        for (int i = 0; i < total; i++) {
            if (labels[i] > 0)
                labels[i] = (uint16_t)uf_find(&uf, labels[i]);
        }

        // Component stats
        memset(comp_area, 0, next_label * sizeof(int));
        memset(comp_sx, 0, next_label * sizeof(int));
        memset(comp_sy, 0, next_label * sizeof(int));
        memset(comp_perim, 0, next_label * sizeof(int));

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = y * width + x;
                uint16_t lbl = labels[idx];
                if (lbl == 0) continue;

                comp_area[lbl]++;
                comp_sx[lbl] += x;
                comp_sy[lbl] += y;

                bool bnd = (x == 0 || x == width - 1 || y == 0 || y == height - 1);
                if (!bnd) {
                    bnd = (labels[idx - 1] != lbl || labels[idx + 1] != lbl ||
                           labels[idx - width] != lbl || labels[idx + width] != lbl);
                }
                if (bnd) comp_perim[lbl]++;
            }
        }

        // Find best circular component
        float best_quality = -1.0f;
        int best_lbl = 0;

        for (int lbl = 1; lbl < next_label; lbl++) {
            if (comp_area[lbl] < MIN_COMPONENT_AREA) continue;
            float p = (float)comp_perim[lbl];
            if (p < 1.0f) continue;
            float circ = 4.0f * (float)M_PI * (float)comp_area[lbl] / (p * p);
            if (circ < MIN_CIRCULARITY) continue;

            float quality = circ * sqrtf((float)comp_area[lbl]);
            if (quality > best_quality) {
                best_quality = quality;
                best_lbl = lbl;
            }
        }

        if (best_lbl > 0) {
            result.center_x = comp_sx[best_lbl] / comp_area[best_lbl];
            result.center_y = comp_sy[best_lbl] / comp_area[best_lbl];
            result.black_radius = sqrtf((float)comp_area[best_lbl] / (float)M_PI);
            result.found = true;

            float p = (float)comp_perim[best_lbl];
            float circ = 4.0f * (float)M_PI * (float)comp_area[best_lbl] / (p * p);
            ESP_LOGI(TAG, "Target: center=(%d,%d) r=%.1f area=%d circ=%.2f thresh=%d tries=%d",
                     result.center_x, result.center_y, result.black_radius,
                     comp_area[best_lbl], circ, threshold, tries);
            break;
        }

        // No target - dynamic threshold search
        if (tries == 0) {
            if (dark_ratio >= 0.02f) {
                ESP_LOGW(TAG, "No target, dark %.1f%% (not overexposed), thresh=%d",
                         dark_ratio * 100, threshold);
                break;
            }
            ESP_LOGI(TAG, "Overexposed (dark %.1f%%), searching upward step=%d max=%d",
                     dark_ratio * 100, step, thresh_max);
        }

        if (dark_ratio >= 0.5f || threshold >= thresh_max) {
            ESP_LOGW(TAG, "Target out of frame (dark %.1f%% thresh=%d tries=%d)",
                     dark_ratio * 100, threshold, tries);
            break;
        }

        threshold += step;
        if (threshold > thresh_max) threshold = thresh_max;
    }

cleanup:
    free(bin);
    free(labels);
    free(uf_parent);
    free(uf_rnk);
    free(comp_area);
    free(comp_sx);
    free(comp_sy);
    free(comp_perim);

    return result;
}
