#ifndef CAMERA_CONTROL_H
#define CAMERA_CONTROL_H

#include <Arduino.h>
#include <WebServer.h>
#include "config.h"

enum class CaptureState
{
    Starting, WarmingUp, Ready, Preparing, Capturing, Saving, Cooldown, Error
};

enum class CaptureError
{
    None, StorageInit, CameraInit, CameraReset, CameraWarmup, Capture, StorageSave
};

// 初期化、ボタン、フレーム、LED、復旧はここで持つ
// GPIO初期化後にbeginCaptureを1回、loopでupdateCapture
void beginCapture();
void updateCapture();
CaptureState captureState();
CaptureError captureError();
const char *captureErrorName(CaptureError reason);

// 旧Web用。standaloneでは未使用
void handlePhoto(WebServer &server);
void handleCameraStream(WebServer &server);

#endif
