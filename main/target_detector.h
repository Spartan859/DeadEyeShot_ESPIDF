#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int center_x;
    int center_y;
    float black_radius;
    bool found;
} target_result_t;

target_result_t target_detect(const uint8_t *gray, int width, int height);
