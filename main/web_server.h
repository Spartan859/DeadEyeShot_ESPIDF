#pragma once

#include <stdint.h>
#include "esp_camera.h"

#define WEB_SERVER_PORT 80

esp_err_t web_server_init(void);
void web_server_update_shot(const uint8_t *jpeg_data, int jpeg_len,
                            int width, int height);
void web_server_update_video_frame(const uint8_t *jpeg_data, int jpeg_len,
                                   int width, int height);
