#pragma once

#include <stdint.h>
#include "target_detector.h"

typedef enum {
    TARGET_TYPE_PISTOL = 1,
    TARGET_TYPE_RIFLE  = 2,
} target_type_t;

typedef struct {
    float score;
    target_type_t type;
} score_result_t;

score_result_t score_calculate(const target_result_t *target,
                               int aim_x, int aim_y,
                               target_type_t type);
