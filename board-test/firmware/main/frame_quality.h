#pragma once
#include "esp_camera.h"
#include "quality_policy.h"
esp_err_t measure_frame(camera_fb_t *frame, frame_quality *quality);
