#include <Arduino.h>
#include "config.h"
#include "led_control.h"
#include "camera_control.h"
#include "wifi_manager.h"

void setup()
{
    // LED初期化（最優先で実行）
    initLED();

    // シリアル通信初期化
    Serial.begin(SERIAL_BAUD);

    // USB接続待機（設定した時間まで）
    unsigned long serial_start = millis();
    while (!Serial && (millis() - serial_start < SERIAL_WAIT_TIMEOUT))
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

    // ウォームアップはloop()で進める
    const bool request_setup = digitalRead(BUTTON_47) == LOW;
    beginCapture();
    beginConnectivity(request_setup);

    Serial.println("Setup complete!");
}

void loop()
{
    static uint32_t lastPrintTime = 0;

    updateCapture();

    // 2秒ごとにハートビート
    if (uint32_t(uint32_t(millis()) - lastPrintTime) > 2000)
    {
        Serial.println("System running...");
        lastPrintTime = millis();
    }

    delay(5);
}
