#pragma once
#include <array>
#include <string>

struct WiFiProfile { std::string ssid, password; };
struct NetworkSettings
{
    std::array<WiFiProfile, 3> wifi;
    size_t wifi_count = 0;
    std::string setup_password = "camera-setup";
    bool backup_enabled = false;
    std::string provider = "webdav";
    std::string url, username, password, ca_pem;
    std::string ntp_server = "pool.ntp.org";
};

bool decodeSettings(const std::string &json, NetworkSettings &out, std::string &error);
std::string encodeSettings(const NetworkSettings &settings, bool include_secrets);
bool applySettingsForm(const std::string &json, const NetworkSettings &current,
                       NetworkSettings &out, std::string &error);
