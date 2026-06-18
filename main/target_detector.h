#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int center_x;
    int center_y;
    float black_radius;
    bool found;
} target_result_t;

// If mask_out is non-NULL, fills it with binary mask (255=dark, 0=light).
// Buffer must be width*height bytes, allocated by caller.
target_result_t target_detect(const uint8_t *gray, int width, int height,
                               uint8_t *mask_out);
