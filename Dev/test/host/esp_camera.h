#pragma once
#include "Arduino.h"
#include <cassert>
constexpr int LEDC_CHANNEL_0=0, LEDC_TIMER_0=0, PIXFORMAT_JPEG=1;
constexpr int FRAMESIZE_UXGA=1, CAMERA_FB_IN_PSRAM=1, CAMERA_GRAB_WHEN_EMPTY=0;
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0;
struct camera_config_t {
    int ledc_channel, ledc_timer;
    int pin_d0, pin_d1, pin_d2, pin_d3, pin_d4, pin_d5, pin_d6, pin_d7;
    int pin_xclk, pin_pclk, pin_vsync, pin_href, pin_sccb_sda, pin_sccb_scl;
    int pin_pwdn, pin_reset, xclk_freq_hz, pixel_format, frame_size;
    int jpeg_quality, fb_count, fb_location, grab_mode;
};
struct camera_fb_t { uint8_t *buf; size_t len; };
inline uint8_t mock_image[]={0xff, 0xd8, 0xff, 0xd9};
inline camera_fb_t mock_frame{mock_image, sizeof(mock_image)};
inline bool mock_camera_ok=true, mock_capture_ok=true, mock_reset_ok=true;
inline int mock_frames_out=0, mock_captures=0;
inline esp_err_t esp_camera_init(camera_config_t *) { return mock_camera_ok ? ESP_OK : -1; }
inline esp_err_t esp_camera_deinit() { assert(mock_frames_out == 0); return mock_reset_ok ? ESP_OK : -1; }
inline camera_fb_t *esp_camera_fb_get() {
    if (!mock_capture_ok) return nullptr;
    ++mock_frames_out;
    ++mock_captures;
    assert(mock_frames_out == 1);
    return &mock_frame;
}
inline void esp_camera_fb_return(camera_fb_t *) { assert(mock_frames_out == 1); --mock_frames_out; }
