#pragma once

#include <stdbool.h>
#include "esp_camera.h"
#include "esp_err.h"

esp_err_t camera_init(void);
esp_err_t camera_deinit(void);
void camera_request_active(bool active);
bool camera_wants_active(void);
bool camera_is_active(void);
camera_fb_t *camera_capture(void);
void camera_return_fb(camera_fb_t *fb);
