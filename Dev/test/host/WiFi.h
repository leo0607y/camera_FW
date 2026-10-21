#pragma once
#include "Arduino.h"
#include <vector>
constexpr int WIFI_STA=1,WIFI_AP_STA=3,WL_CONNECTED=3,WIFI_SCAN_FAILED=-2,WIFI_SCAN_RUNNING=-1;
inline int mock_wifi_status=0,mock_scan=WIFI_SCAN_FAILED;
inline bool mock_ap=false;
inline std::vector<std::string> mock_wifi_attempts;
struct MockWiFi {
    void persistent(bool) {}
    void setAutoReconnect(bool) {}
    void mode(int) {}
    bool softAP(const char *,const char *) { mock_ap=true;return true; }
    void softAPdisconnect(bool) { mock_ap=false; }
    int softAPIP() { return 1; }
    int status() { return mock_wifi_status; }
    String SSID() { return mock_wifi_attempts.empty() ? "Home" : mock_wifi_attempts.back(); }
    String SSID(int index) { return index==0 ? "Home" : "Phone"; }
    int RSSI(int) { return -50; }
    int scanComplete() { return mock_scan; }
    void scanDelete() { mock_scan=WIFI_SCAN_FAILED; }
    void scanNetworks(bool) { mock_scan=2; }
    void disconnect(bool,bool,unsigned long) { mock_wifi_status=0; }
    void begin(const char *ssid,const char *) { mock_wifi_attempts.emplace_back(ssid); }
};
inline MockWiFi WiFi;
