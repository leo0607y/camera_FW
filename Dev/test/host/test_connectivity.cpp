#include "wifi_manager.h"
#include "network_settings.h"
#include "camera_control.h"
#include "sd_card.h"
#include "led_control.h"
#include "backup_store.h"
#include "backup_transport.h"
#include "WiFi.h"
#include "Preferences.h"
#include "esp_http_client.h"
#include "freertos/task.h"
#include <ArduinoJson.h>
#include <cassert>
#include <iostream>

CaptureState captureState() { return CaptureState::Ready; }
static void request(const char *path,int method,const std::string &body="") {
    mock_body=body;mock_response_code=0;mock_routes.at({path,method})();
}
static NetworkSettings configured(bool backup=false) {
    NetworkSettings s;s.wifi_count=2;s.wifi[0]={"Home","home-password"};s.wifi[1]={"Office","office-password"};
    s.setup_password="my-setup-password";s.backup_enabled=backup;
    s.url="https://example.test/dav";s.username="person";s.password="app-password";
    return s;
}
int main(int argc,char **argv) {
    assert(argc==2);const std::string scenario=argv[1];
    initLED();assert(initSDCard());
    updateCameraStatusLED(true,false);
    const bool fresh=scenario=="setup_first_boot" || scenario=="settings_api_save" || scenario=="settings_api_write_failure";
    if (!fresh) mock_saved_settings=encodeSettings(configured(scenario=="automatic_backup"),true);
    const bool force=scenario.rfind("settings_api",0)==0 || scenario=="setup_access_control" || scenario=="setup_timeout";
    if (scenario=="automatic_backup") { mock_wifi_status=WL_CONNECTED;mock_files["/1.jpg"]={1,2,3,4}; }
    beginConnectivity(force);
    mock_task_limit=8;
    mock_task_hook=[&](int tick) {
        if (scenario=="setup_first_boot") assert(mock_ap);
        else if (scenario=="setup_access_control" && tick==0) {
            request("/api/settings",HTTP_GET);assert(mock_response_code==200);
            assert(mock_response.find("home-password")==std::string::npos);
            assert(mock_response.find("app-password")==std::string::npos);
            mock_local_ip=2;request("/api/settings",HTTP_GET);assert(mock_response_code==403);
            mock_local_ip=1;request("/api/settings",HTTP_POST,"{}");assert(mock_response_code==403);
        } else if ((scenario=="settings_api_save" || scenario=="settings_api_write_failure") && tick==0) {
            request("/api/settings",HTTP_GET);assert(mock_response_code==200);
            JsonDocument doc;assert(!deserializeJson(doc,mock_response));
            mock_request_headers["X-Setup-Token"]=doc["csrf"].as<std::string>();
            if (scenario=="settings_api_write_failure") mock_preferences_ok=false;
            request("/api/settings",HTTP_POST,R"({"wifi":{"ssid":"New WiFi","password":"new-password"},"setup_password":"new-setup-password"})");
            assert(mock_response_code==(mock_preferences_ok ? 200 : 500));
            if (mock_preferences_ok) {
                NetworkSettings saved;std::string error;
                assert(decodeSettings(mock_saved_settings,saved,error));
                assert(saved.wifi[0].ssid=="New WiFi");
            } else assert(mock_saved_settings.empty());
        } else if (scenario=="setup_timeout") {
            if (tick==0) { assert(mock_ap);mock_time+=600001; }
            if (tick==1) assert(!mock_ap);
        } else if (scenario=="wifi_failover") {
            mock_time+=21000;
            if (tick==7) {
                assert(mock_wifi_attempts.size()>=2);
                assert(mock_wifi_attempts[0]=="Home" && mock_wifi_attempts[1]=="Office");
                assert(mock_ap);
            }
        } else if (scenario=="wifi_reconnect") {
            if (tick==0) mock_wifi_status=WL_CONNECTED;
            if (tick==1) { assert(led_states[0]==1 && led_states[LED_READY_INDEX]==1);mock_wifi_status=0; }
            if (tick==2) { assert(led_states[0]==0 && led_states[LED_READY_INDEX]==1);assert(mock_wifi_attempts.size()>=2); }
        } else if (scenario=="automatic_backup") {
            mock_time+=1000;
            if (tick==7) {
                assert(mock_put_calls==1 && mock_get_calls==1);
                assert(mock_files.at("/1.jpg")==std::vector<uint8_t>({1,2,3,4}));
                const std::string digest=backupDigest(mock_remote.data(),mock_remote.size());
                assert(hasBackupReceipt(backupDestinationId(configured(true)),digest));
            }
        }
    };
    try { mock_task(mock_task_argument); } catch (const MockTaskFinished &) {}
    if (scenario=="settings_api_save") assert(!mock_wifi_attempts.empty() && mock_wifi_attempts.back()=="New WiFi");
    std::cout<<"PASS "<<scenario<<'\n';
}
