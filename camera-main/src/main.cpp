#include <Arduino.h>
#include <WebServer.h>
#include "config.h"
#include "led_control.h"
#include "camera_control.h"
#include "sd_card.h"

// Webサーバーインスタンス
WebServer server(WEB_SERVER_PORT);

void setup()
{
    // LED初期化（最優先で実行）
    initLED();

    // シリアル通信初期化
    Serial.begin(SERIAL_BAUD);

    // USB接続待機（最大3秒）
    unsigned long serial_start = millis();
    while (!Serial && (millis() - serial_start < 3000))
    {
        delay(10);
    }
    delay(100);

    Serial.write(13);
    Serial.write(10);
    Serial.println("START");
    Serial.printf("Device: %s v%s\n", DEVICE_NAME, DEVICE_VERSION);
    Serial.println("[LED] All LEDs initialized to OFF");

    // LED状態を確認
    for (int i = 0; i < NUM_LEDS; i++)
    {
        Serial.printf("[LED] GPIO %d: %s\n", LED_PINS[i],
                      digitalRead(LED_PINS[i]) == HIGH ? "OFF" : "ON");
    }

    // ボタン初期化
    pinMode(BUTTON_47, INPUT_PULLUP);
    pinMode(BUTTON_48, INPUT_PULLUP);
    Serial.printf("Button GPIO %d, %d initialized\n", BUTTON_47, BUTTON_48);

    // SDカード初期化
    if (initSDCard())
    {
        Serial.println("[SD] SD card ready");
    }
    else
    {
        Serial.println("[SD] SD card initialization failed - photos will not be saved");
    }

    // カメラ初期化
    initCamera();

    // カメラウォームアップ（起動時の5フレーム自動破棄）
    warmupCamera();

    Serial.println("Setup complete!");
}

void loop()
{
    static unsigned long lastPrintTime = 0;

    // カメラボタン処理
    handleCameraButton();

    // 撮影状態によるLED更新（撮影中はhandleCameraButton内で制御される）
    if (camera_error_state)
    {
        // エラー状態: IO38点灯、IO46消灯
        updateCameraStatusLED(false, true);
    }
    else
    {
        // 通常状態: 撮影可能かチェック
        bool ready_to_shoot = (millis() - last_photo_time >= PHOTO_COOLDOWN_MS);
        updateCameraStatusLED(ready_to_shoot, false);
    }

    // 2秒ごとにハートビート
    if (millis() - lastPrintTime > 2000)
    {
        Serial.println("System running...");
        lastPrintTime = millis();
    }

    delay(50);
}