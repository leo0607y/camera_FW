#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>
#include <WebServer.h>
#include "config.h"

// LED状態管理
extern int led_states[NUM_LEDS];

// LED初期化
void initLED();

// Wi-Fi接続状態によるLED表示
void setWiFiStatusLED(bool connected);

// 撮影状態によるLED表示
void updateCameraStatusLED(bool ready, bool error);

// WebハンドラによるLED制御
void handleLEDStatus(WebServer &server);
void handleLEDOn(WebServer &server);
void handleLEDOff(WebServer &server);

#endif
