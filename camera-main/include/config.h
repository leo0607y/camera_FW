#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =============== Wi-Fi設定 ===============
extern const char *WIFI_SSID;
extern const char *WIFI_PASSWORD;

// =============== Webサーバー設定 ===============
const uint16_t WEB_SERVER_PORT = 80;

// =============== LED設定 ===============
const int LED_PINS[] = {1, 21, 38, 45, 46};
const int NUM_LEDS = sizeof(LED_PINS) / sizeof(LED_PINS[0]);
const int LED_45_INDEX = 3;
const int LED_46_INDEX = 4;

// =============== LED役割定義 ===============
const int LED_ERROR_INDEX = 2; // IO38: エラー表示用
const int LED_READY_INDEX = 4; // IO46: 撮影可能表示用

// =============== 撮影制御設定 ===============
const unsigned long PHOTO_COOLDOWN_MS = 500; // 撮影間隔（ミリ秒）

// =============== ボタン設定 ===============
const int BUTTON_47 = 47;
const int BUTTON_48 = 48;

// =============== シリアル設定 ===============
const uint32_t SERIAL_BAUD = 115200;
const uint32_t SERIAL_WAIT_TIMEOUT = 5000; // ミリ秒

// =============== デバイス設定 ===============
extern const char *DEVICE_NAME;
extern const char *DEVICE_VERSION;

// =============== カメラピン設定 ===============
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM 42
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5

#define Y2_GPIO_NUM 40
#define Y3_GPIO_NUM 9
#define Y4_GPIO_NUM 8
#define Y5_GPIO_NUM 41
#define Y6_GPIO_NUM 3
#define Y7_GPIO_NUM 18
#define Y8_GPIO_NUM 17
#define Y9_GPIO_NUM 16

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 39

// =============== カメラ設定 ===============
#define CAMERA_XCLK_FREQ 20000000
#define CAMERA_PIXEL_FORMAT PIXFORMAT_JPEG
#define CAMERA_FRAME_SIZE FRAMESIZE_UXGA
#define CAMERA_JPEG_QUALITY 10
#define CAMERA_FB_COUNT 2

// =============== SDカードピン設定 ===============
#define SD_CS 10
#define SD_MOSI 11
#define SD_MISO 13
#define SD_SCLK 12

#endif
