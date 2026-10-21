#include "led_control.h"

// LED状態管理
int led_states[NUM_LEDS] = {0}; // 0 = OFF (HIGH), 1 = ON (LOW)

void initLED()
{
    // すべてのLEDピンを出力モードに設定し、消灯
    for (int i = 0; i < NUM_LEDS; i++)
    {
        pinMode(LED_PINS[i], OUTPUT);
        digitalWrite(LED_PINS[i], HIGH); // Active-low: HIGH = OFF (消灯)
        delayMicroseconds(100);          // 安定化のための短い遅延
    }

    // 再度すべて消灯（確実に）
    for (int i = 0; i < NUM_LEDS; i++)
    {
        digitalWrite(LED_PINS[i], HIGH);
    }
}

void setWiFiStatusLED(bool connected)
{
    if (connected)
    {
        // Wi-Fi接続成功時: IO1のみ点灯
        digitalWrite(LED_PINS[0], LOW); // GPIO 1 ON
        Serial.println("[LED] IO1 ON (Wi-Fi Connected)");

        for (int i = 1; i < NUM_LEDS; i++)
        {
            digitalWrite(LED_PINS[i], HIGH); // その他のLEDは消灯
        }
        Serial.println("[LED] IO21, IO38, IO45, IO46 OFF");
    }
    else
    {
        // Wi-Fi接続失敗時: すべてのLEDを消灯
        for (int i = 0; i < NUM_LEDS; i++)
        {
            digitalWrite(LED_PINS[i], HIGH);
        }
        Serial.println("[LED] All LEDs OFF (Wi-Fi Failed)");
    }
}

void updateCameraStatusLED(bool ready, bool error)
{
    // IO46: 撮影可能表示
    if (ready && !error)
    {
        digitalWrite(LED_PINS[LED_READY_INDEX], LOW); // ON
        Serial.println("[LED] IO46 ON (Ready to shoot)");
    }
    else
    {
        digitalWrite(LED_PINS[LED_READY_INDEX], HIGH); // OFF
        Serial.println("[LED] IO46 OFF (Not ready)");
    }

    // IO38: エラー表示
    if (error)
    {
        digitalWrite(LED_PINS[LED_ERROR_INDEX], LOW); // ON
        Serial.println("[LED] IO38 ON (Error state)");
    }
    else
    {
        digitalWrite(LED_PINS[LED_ERROR_INDEX], HIGH); // OFF
        Serial.println("[LED] IO38 OFF (No error)");
    }
}

void handleLEDStatus(WebServer &server)
{
    String json = "{\"leds\":[";
    for (int i = 0; i < NUM_LEDS; i++)
    {
        if (i > 0)
            json += ",";
        json += "{\"gpio\":" + String(LED_PINS[i]) + ",\"state\":" + String(led_states[i]) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleLEDOn(WebServer &server)
{
    for (int i = 0; i < NUM_LEDS; i++)
    {
        digitalWrite(LED_PINS[i], LOW); // Active-low: LOW = ON
        led_states[i] = 1;              // 1 = ON
    }
    Serial.println("[Web] All LEDs ON");
    server.send(200, "text/plain", "OK");
}

void handleLEDOff(WebServer &server)
{
    for (int i = 0; i < NUM_LEDS; i++)
    {
        digitalWrite(LED_PINS[i], HIGH); // Active-low: HIGH = OFF
        led_states[i] = 0;               // 0 = OFF
    }
    Serial.println("[Web] All LEDs OFF");
    server.send(200, "text/plain", "OK");
}
