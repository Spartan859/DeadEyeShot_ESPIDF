#include "scorer.h"
#include <math.h>

score_result_t score_calculate(const target_result_t *target,
                               int aim_x, int aim_y,
                               target_type_t type)
{
    score_result_t result = {0};
    result.type = type;

    if (!target || !target->found) {
        result.score = 0.0f;
        return result;
    }

    float dx = (float)(aim_x - target->center_x);
    float dy = (float)(aim_y - target->center_y);
    float distance = sqrtf(dx * dx + dy * dy);

    if (target->ring_width_pixels < 1.0f) {
        result.score = 10.0f;
        return result;
    }

    float score = 10.0f - (distance / target->ring_width_pixels);

    if (score > 10.9f) score = 10.9f;
    if (score < 1.0f)  score = 1.0f;

    result.score = score;
    return result;
}
