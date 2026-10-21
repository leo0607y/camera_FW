#include "led_control.h"

// LED状態管理
int led_states[NUM_LEDS] = {0}; // 0 = OFF (HIGH), 1 = ON (LOW)

void initLED()
{
    // すべてのLEDピンを出力モードに設定し、消灯
    for (int i = 0; i < NUM_LEDS; i++)
    {
        led_states[i] = 0;
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
    // Wi-FiはIO1だけ。撮影・エラーのLEDは触らない
    digitalWrite(LED_PINS[0], connected ? LOW : HIGH);
    led_states[0] = connected ? 1 : 0;
}

void updateCameraStatusLED(bool ready, bool error)
{
    const int ready_state = ready && !error ? 1 : 0;
    const int error_state = error ? 1 : 0;
    const bool changed = led_states[LED_READY_INDEX] != ready_state ||
                         led_states[LED_ERROR_INDEX] != error_state;
    digitalWrite(LED_PINS[LED_READY_INDEX], ready_state ? LOW : HIGH);
    digitalWrite(LED_PINS[LED_ERROR_INDEX], error_state ? LOW : HIGH);
    led_states[LED_READY_INDEX] = ready_state;
    led_states[LED_ERROR_INDEX] = error_state;
    if (changed)
    {
        Serial.printf("[LED] Ready: %d, Error: %d\n", ready_state, error_state);
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
