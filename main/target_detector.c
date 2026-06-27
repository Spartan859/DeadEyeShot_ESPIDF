#include "target_detector.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "target_det";

typedef struct {
    bool ok;
    float radius;
    float contrast;
    float support;
    const char *reason;
} radial_result_t;

typedef struct {
    float cx;
    float cy;
    int area;
    int perimeter;
    float circularity;
    float radius_area;
    float radius_box;
    float radius_min;
    float score;
} candidate_t;

static target_result_t empty_detection(const char *reason)
{
    target_result_t result = {0};
    result.reason = reason ? reason : "no_shot";
    result.method = "none";
    return result;
}

static float clampf_local(float value, float min_value, float max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int clampi_local(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void sort_float(float *values, int count)
{
    for (int i = 1; i < count; i++) {
        float value = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > value) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = value;
    }
}

static float median_float(float *values, int count)
{
    if (count <= 0) return 0.0f;
    sort_float(values, count);
    int mid = count / 2;
    if ((count % 2) == 1) return values[mid];
    return (values[mid - 1] + values[mid]) * 0.5f;
}

static uint8_t sample_gray(const uint8_t *gray, int width, int height, float x, float y)
{
    int ix = clampi_local((int)lroundf(x), 0, width - 1);
    int iy = clampi_local((int)lroundf(y), 0, height - 1);
    return gray[iy * width + ix];
}

static radial_result_t radial_refine(const uint8_t *gray, int width, int height,
                                     float cx, float cy, float guess_radius)
{
    radial_result_t result = {0};
    result.reason = "ok";

    float edge_min = fminf(fminf(cx, cy), fminf((float)(width - 1) - cx, (float)(height - 1) - cy));
    int max_radius = (int)floorf(edge_min);
    int search_max = (int)floorf(fminf((float)max_radius, fmaxf(guess_radius * 1.7f, guess_radius + 18.0f)));
    int search_min = (int)floorf(fmaxf(4.0f, guess_radius * 0.45f));
    if (search_max <= search_min + 4) {
        result.reason = "radial_range";
        return result;
    }

    float radii[64];
    float contrasts[64];
    int radii_count = 0;
    int contrast_count = 0;
    const int directions = 64;

    for (int i = 0; i < directions; i++) {
        float angle = ((float)M_PI * 2.0f * (float)i) / (float)directions;
        float dx = cosf(angle);
        float dy = sinf(angle);
        int best_radius = 0;
        int best_rise = 0;
        for (int r = search_min; r <= search_max; r++) {
            uint8_t inner = sample_gray(gray, width, height,
                                        cx + dx * fmaxf(0.0f, (float)r - 3.0f),
                                        cy + dy * fmaxf(0.0f, (float)r - 3.0f));
            uint8_t outer = sample_gray(gray, width, height,
                                        cx + dx * ((float)r + 3.0f),
                                        cy + dy * ((float)r + 3.0f));
            int rise = (int)outer - (int)inner;
            if (rise > best_rise) {
                best_rise = rise;
                best_radius = r;
            }
        }
        if (best_radius > 0 && best_rise >= 8) {
            radii[radii_count++] = (float)best_radius;
            contrasts[contrast_count++] = (float)best_rise;
        }
    }

    float contrast_values[64];
    memcpy(contrast_values, contrasts, sizeof(float) * contrast_count);
    if (radii_count < (int)floorf((float)directions * 0.35f)) {
        result.contrast = median_float(contrast_values, contrast_count);
        result.support = (float)radii_count / (float)directions;
        result.reason = "radial_support_raw";
        return result;
    }

    float radii_for_median[64];
    memcpy(radii_for_median, radii, sizeof(float) * radii_count);
    float radius_median = median_float(radii_for_median, radii_count);
    float filtered[64];
    int filtered_count = 0;
    for (int i = 0; i < radii_count; i++) {
        if (fabsf(radii[i] - radius_median) <= fmaxf(3.0f, radius_median * 0.18f)) {
            filtered[filtered_count++] = radii[i];
        }
    }
    if (filtered_count < (int)floorf((float)directions * 0.3f)) {
        result.radius = radius_median;
        result.contrast = median_float(contrast_values, contrast_count);
        result.support = (float)filtered_count / (float)directions;
        result.reason = "radial_support_filtered";
        return result;
    }

    result.ok = true;
    result.radius = median_float(filtered, filtered_count);
    result.contrast = median_float(contrast_values, contrast_count);
    result.support = (float)filtered_count / (float)directions;
    result.reason = "ok";
    return result;
}

static float score_candidate(const candidate_t *candidate, const radial_result_t *radial,
                             int width, int height)
{
    float radius_consistency = 1.0f - fminf(0.6f, fabsf(radial->radius - candidate->radius_area) /
                                            fmaxf(fmaxf(radial->radius, candidate->radius_area), 1.0f));
    float contrast_factor = clampf_local(radial->contrast / 55.0f, 0.25f, 1.25f);
    float support_factor = clampf_local(radial->support, 0.25f, 1.0f);
    float border_margin = fminf(fminf(candidate->cx, candidate->cy),
                                fminf((float)width - candidate->cx, (float)height - candidate->cy));
    float border_factor = border_margin < radial->radius * 0.65f ? 0.65f : 1.0f;
    return candidate->circularity * sqrtf((float)candidate->area) *
           radius_consistency * contrast_factor * support_factor * border_factor;
}

static float confidence_for_candidate(const candidate_t *candidate, const radial_result_t *radial)
{
    float radius_consistency = 1.0f - fminf(0.6f, fabsf(radial->radius - candidate->radius_area) /
                                            fmaxf(fmaxf(radial->radius, candidate->radius_area), 1.0f));
    float circularity_factor = clampf_local((candidate->circularity - 0.45f) / 0.35f, 0.0f, 1.0f);
    float contrast_factor = clampf_local(radial->contrast / 35.0f, 0.0f, 1.0f);
    float support_factor = clampf_local((radial->support - 0.35f) / 0.45f, 0.0f, 1.0f);
    return clampf_local(0.15f + circularity_factor * 0.3f + radius_consistency * 0.25f +
                        contrast_factor * 0.15f + support_factor * 0.15f, 0.0f, 1.0f);
}

static float fill_mask(const uint8_t *gray, uint8_t *mask, int total, float threshold)
{
    int count = 0;
    for (int i = 0; i < total; i++) {
        mask[i] = gray[i] < threshold ? 1 : 0;
        if (mask[i]) count++;
    }
    return total == 0 ? 0.0f : (float)count / (float)total;
}

static target_result_t detect_best_component_in_mask(const uint8_t *mask, const uint8_t *gray,
                                                     uint8_t *visited, uint32_t *queue,
                                                     int width, int height, float max_radius)
{
    memset(visited, 0, (size_t)width * (size_t)height);
    target_result_t best = empty_detection("no_target");
    float best_score = 0.0f;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int start = y * width + x;
            if (mask[start] == 0 || visited[start] != 0) continue;

            int head = 0;
            int tail = 0;
            queue[tail++] = (uint32_t)start;
            visited[start] = 1;

            int area = 0;
            double sum_x = 0.0;
            double sum_y = 0.0;
            int perimeter = 0;
            int min_x = x, max_x = x, min_y = y, max_y = y;

            while (head < tail) {
                int idx = (int)queue[head++];
                int px = idx % width;
                int py = idx / width;
                area++;
                sum_x += px;
                sum_y += py;
                if (px < min_x) min_x = px;
                if (px > max_x) max_x = px;
                if (py < min_y) min_y = py;
                if (py > max_y) max_y = py;

                const int nx[4] = { px - 1, px + 1, px, px };
                const int ny[4] = { py, py, py - 1, py + 1 };
                for (int i = 0; i < 4; i++) {
                    if (nx[i] < 0 || nx[i] >= width || ny[i] < 0 || ny[i] >= height) {
                        perimeter++;
                        continue;
                    }
                    int ni = ny[i] * width + nx[i];
                    if (mask[ni] == 0) {
                        perimeter++;
                        continue;
                    }
                    if (visited[ni] == 0) {
                        visited[ni] = 1;
                        queue[tail++] = (uint32_t)ni;
                    }
                }
            }

            if (perimeter <= 0) continue;
            int comp_width = max_x - min_x + 1;
            int comp_height = max_y - min_y + 1;
            if (comp_width < 3 || comp_height < 3) continue;

            float aspect = (float)fminf((float)comp_width, (float)comp_height) /
                           (float)fmaxf((float)comp_width, (float)comp_height);
            if (aspect < 0.82f) continue;

            float circularity = (4.0f * (float)M_PI * (float)area) / ((float)perimeter * (float)perimeter);
            if (circularity < 0.5f) continue;

            float radius_area = sqrtf((float)area / (float)M_PI);
            float radius_box = ((float)comp_width + (float)comp_height) / 4.0f;
            float radius_min = (float)fminf((float)comp_width, (float)comp_height) / 2.0f;
            if (radius_area > max_radius || radius_min > max_radius * 1.15f) continue;

            float radius_consistency = fminf(radius_area, radius_box) / fmaxf(radius_area, radius_box);
            if (radius_consistency < 0.72f) continue;

            candidate_t candidate = {
                .cx = (float)(sum_x / (double)area),
                .cy = (float)(sum_y / (double)area),
                .area = area,
                .perimeter = perimeter,
                .circularity = circularity,
                .radius_area = radius_area,
                .radius_box = radius_box,
                .radius_min = radius_min,
                .score = 0.0f
            };

            radial_result_t radial = radial_refine(gray, width, height, candidate.cx, candidate.cy, radius_min);
            if (!radial.ok) continue;
            if (radial.support < 0.75f) continue;

            candidate.score = score_candidate(&candidate, &radial, width, height);
            if (candidate.score > best_score) {
                best_score = candidate.score;
                best.found = true;
                best.center_x = (int)lroundf(candidate.cx);
                best.center_y = (int)lroundf(candidate.cy);
                best.black_radius = radial.radius;
                best.confidence = confidence_for_candidate(&candidate, &radial);
                best.reason = "ok";
                best.method = "component_radial";
            }
        }
    }

    if (best.found && best.confidence >= 0.25f) return best;
    return best.found ? empty_detection("low_confidence") : empty_detection("no_target");
}

target_result_t target_detect(const uint8_t *gray, int width, int height, uint8_t *mask_out)
{
    if (!gray || width <= 0 || height <= 0) {
        return empty_detection("no_shot");
    }

    int total = width * height;
    double sum = 0.0;
    for (int i = 0; i < total; i++) sum += gray[i];
    float mean = total == 0 ? 0.0f : (float)(sum / (double)total);

    uint8_t *mask = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *visited = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *queue = (uint32_t *)heap_caps_malloc((size_t)total * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mask || !visited || !queue) {
        ESP_LOGE(TAG, "OOM in JS-equivalent target_detect (%d pixels)", total);
        free(mask);
        free(visited);
        free(queue);
        return empty_detection("no_target");
    }

    target_result_t best = empty_detection("no_target");
    float threshold = mean / 2.0f;
    float step = fmaxf(5.0f, mean * 0.05f);
    float max_threshold = fmaxf(threshold, mean * 1.15f);
    bool have_debug_mask = false;

    while (threshold <= max_threshold + 0.001f) {
        float ratio = fill_mask(gray, mask, total, threshold);
        if (ratio >= 0.001f) {
            have_debug_mask = true;
            float max_radius = fminf((float)width, (float)height) * 0.48f;
            target_result_t result = detect_best_component_in_mask(mask, gray, visited, queue, width, height, max_radius);
            if (result.found && result.confidence > best.confidence) {
                best = result;
                if (mask_out) {
                    for (int i = 0; i < total; i++) mask_out[i] = mask[i] ? 255 : 0;
                }
            }
        } else if (mask_out && !have_debug_mask) {
            for (int i = 0; i < total; i++) mask_out[i] = mask[i] ? 255 : 0;
        }
        threshold += step;
    }

    free(mask);
    free(visited);
    free(queue);
    return best.found ? best : empty_detection("no_target");
}
