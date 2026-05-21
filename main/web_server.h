#pragma once

#include <stdint.h>
#include "esp_camera.h"

#define WEB_SERVER_PORT 80

esp_err_t web_server_init(void);
void web_server_update_shot(const uint8_t *jpeg_data, int jpeg_len,
                            int width, int height, float score,
                            int target_cx, int target_cy,
                            float black_radius,
                            int aim_x, int aim_y);
