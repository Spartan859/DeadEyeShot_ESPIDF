#include "scorer.h"
#include <math.h>

// Pistol: black circle contains rings 7,8,9,10 → 4 rings
// Rifle:  black circle contains rings 4,5,6,7,8,9,10 → 7 rings
#define PISTOL_RINGS_IN_BLACK 4
#define RIFLE_RINGS_IN_BLACK  7

score_result_t score_calculate(const target_result_t *target,
                               int aim_x, int aim_y,
                               target_type_t type)
{
    score_result_t result = {0};
    result.type = type;

    if (!target || !target->found || target->black_radius < 1.0f) {
        result.score = 0.0f;
        return result;
    }

    float dx = (float)(aim_x - target->center_x);
    float dy = (float)(aim_y - target->center_y);
    float distance = sqrtf(dx * dx + dy * dy);

    int rings_in_black = (type == TARGET_TYPE_PISTOL)
                         ? PISTOL_RINGS_IN_BLACK
                         : RIFLE_RINGS_IN_BLACK;

    // Each ring width = black_radius / rings_in_black
    // score = 10 - (distance / ring_width)
    float ring_width = target->black_radius / rings_in_black;
    float score = 10.0f - (distance / ring_width);

    if (score > 10.9f) score = 10.9f;
    if (score < 1.0f)  score = 1.0f;

    result.score = score;
    return result;
}
