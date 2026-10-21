#include "wifi_manager.h"

bool connectWiFi()
{
    Serial.println("\nConnecting to Wi-Fi...");
    Serial.printf("SSID: %s\n", WIFI_SSID);
    Serial.printf("Password length: %d\n", strlen(WIFI_PASSWORD));

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true); // 前の接続をリセット
    delay(1000);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 40)
    {
        delay(500);
        Serial.print(".");
        wifi_attempts++;

        // 5秒ごとにステータスを表示
        if (wifi_attempts % 10 == 0)
        {
            Serial.printf("\nStatus: %d (0=IDLE, 1=NO_SSID, 2=SCAN_COMPLETE, 3=CONNECTED, 4=CONNECT_FAILED, 5=CONNECTION_LOST, 6=DISCONNECTED)\n", WiFi.status());
        }
    }

    Serial.println("");
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\nWi-Fi Connected!");
        Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
        return true;
    }
    else
    {
        Serial.println("\nWi-Fi Connection Failed!");
        Serial.printf("Final Status: %d\n", WiFi.status());
        Serial.println("Possible causes:");
        Serial.println("- Check SSID and Password in config.h");
        Serial.println("- Check Wi-Fi signal strength");
        Serial.println("- Restart Wi-Fi router");
        return false;
    }
}
