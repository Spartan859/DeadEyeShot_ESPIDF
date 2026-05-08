#pragma once

#include "esp_camera.h"

esp_err_t camera_init(void);
camera_fb_t *camera_capture(void);
void camera_return_fb(camera_fb_t *fb);
