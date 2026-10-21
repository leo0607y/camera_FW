#ifndef CAMERA_CONTROL_H
#define CAMERA_CONTROL_H

#include <Arduino.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "config.h"

// カメラバッファとステータス
extern camera_fb_t *fb;
extern bool camera_initialized;

// 撮影状態管理
extern bool camera_error_state;
extern unsigned long last_photo_time;

// カメラ初期化
void initCamera();

// カメラウォームアップ（起動時のAE/AWB安定化）
void warmupCamera();

// ボタンでカメラ撮影
void handleCameraButton();

// Webハンドラ
void handlePhoto(WebServer &server);
void handleCameraStream(WebServer &server);

#endif
