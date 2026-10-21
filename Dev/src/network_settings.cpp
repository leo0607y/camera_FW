#include "network_settings.h"
#include <ArduinoJson.h>
#include <algorithm>

namespace {
bool clean(const std::string &s) {
    return std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool validate(const NetworkSettings &s, std::string &error, bool saving) {
    if (s.wifi_count > s.wifi.size()) { error="Wi-Fiは3件まで登録できます。"; return false; }
    for (size_t i=0; i<s.wifi_count; ++i) {
        const auto &w=s.wifi[i];
        if (w.ssid.empty() || w.ssid.size()>32 || !clean(w.ssid) || !clean(w.password) ||
            (!w.password.empty() && (w.password.size()<8 || w.password.size()>63))) {
            error="Wi-Fi名またはパスワードの長さ・文字を確認してください。"; return false;
        }
    }
    if (s.setup_password.size()<12 || s.setup_password.size()>63 || !clean(s.setup_password) ||
        (saving && s.setup_password=="camera-setup")) {
        error="設定用Wi-Fiの新しいパスワードを12～63文字で指定してください。"; return false;
    }
    if (s.provider!="webdav") { error="現在のバックアップ方式はWebDAVのみ対応しています。"; return false; }
    if (!s.url.empty() && (s.url.compare(0,8,"https://")!=0 || s.url.size()>512 ||
        !clean(s.url) || s.url.find_first_of("@?#\\ ")!=std::string::npos ||
        s.url.size()<=8 || s.url[8]=='/')) {
        error="保存先は認証情報を含まないHTTPSのWebDAVフォルダURLにしてください。"; return false;
    }
    if (s.backup_enabled && (s.wifi_count==0 || s.url.empty())) {
        error="自動バックアップにはWi-Fiと保存先URLが必要です。"; return false;
    }
    if (s.username.size()>128 || s.username.find(':')!=std::string::npos ||
        s.password.size()>256 || !clean(s.username) || !clean(s.password)) {
        error="バックアップ先の認証情報を確認してください。"; return false;
    }
    if (s.ntp_server.empty() || s.ntp_server.size()>128 || !clean(s.ntp_server) ||
        s.ntp_server.find_first_of(" /\\@?#")!=std::string::npos) {
        error="時刻サーバーにはホスト名かIPアドレスを指定してください。"; return false;
    }
    if (s.ca_pem.size()>4096 || (!s.ca_pem.empty() && s.ca_pem.find("-----BEGIN CERTIFICATE-----")==std::string::npos)) {
        error="CA証明書はPEM形式、4096バイト以内で指定してください。"; return false;
    }
    return true;
}
bool parse(const std::string &json, JsonDocument &doc, std::string &error) {
    if (json.size()>8192 || deserializeJson(doc,json,DeserializationOption::NestingLimit(5)) || !doc.is<JsonObject>()) {
        error="設定データの形式またはサイズが不正です。"; return false;
    }
    return true;
}
void readBackup(JsonVariantConst b, NetworkSettings &s) {
    s.backup_enabled=b["enabled"] | false;
    s.provider=b["provider"] | "webdav";
    s.url=b["url"] | "";
    while (!s.url.empty() && s.url.back()=='/') s.url.pop_back();
    s.username=b["username"] | "";
    s.password=b["password"] | "";
    s.ca_pem=b["ca_pem"] | "";
    s.ntp_server=b["ntp_server"] | "pool.ntp.org";
}
}

bool decodeSettings(const std::string &json, NetworkSettings &out, std::string &error) {
    JsonDocument doc;
    if (!parse(json,doc,error) || doc["version"].as<int>()!=1 || !doc["wifi"].is<JsonArray>()) return false;
    NetworkSettings s;
    for (JsonObjectConst w:doc["wifi"].as<JsonArrayConst>()) {
        if (s.wifi_count==s.wifi.size()) { error="Wi-Fi設定の件数が不正です。"; return false; }
        s.wifi[s.wifi_count++]={w["ssid"] | "",w["password"] | ""};
    }
    s.setup_password=doc["setup_password"] | "";
    readBackup(doc["backup"],s);
    if (!validate(s,error,false)) return false;
    out=s;
    return true;
}

std::string encodeSettings(const NetworkSettings &s, bool include_secrets) {
    JsonDocument doc;
    doc["version"]=1;
    auto wifi=doc["wifi"].to<JsonArray>();
    for (size_t i=0; i<s.wifi_count; ++i) {
        auto w=wifi.add<JsonObject>();
        w["ssid"]=s.wifi[i].ssid;
        w["has_password"]=!s.wifi[i].password.empty();
        if (include_secrets) w["password"]=s.wifi[i].password;
    }
    if (include_secrets) doc["setup_password"]=s.setup_password;
    doc["change_setup_password"]=s.setup_password=="camera-setup";
    auto b=doc["backup"].to<JsonObject>();
    b["enabled"]=s.backup_enabled; b["provider"]=s.provider; b["url"]=s.url;
    b["username"]=s.username; b["ca_pem"]=s.ca_pem; b["ntp_server"]=s.ntp_server;
    b["has_password"]=!s.password.empty();
    if (include_secrets) b["password"]=s.password;
    std::string json;
    serializeJson(doc,json);
    return json;
}

bool applySettingsForm(const std::string &json, const NetworkSettings &current,
                       NetworkSettings &out, std::string &error) {
    JsonDocument doc;
    if (!parse(json,doc,error)) return false;
    NetworkSettings s=current;
    const std::string setup=doc["setup_password"] | "";
    if (!setup.empty()) s.setup_password=setup;
    if (doc["wifi"].is<JsonObject>()) {
        const std::string ssid=doc["wifi"]["ssid"] | "";
        std::string password=doc["wifi"]["password"] | "";
        bool known=false;
        for (size_t i=0;i<current.wifi_count;++i) known=known || current.wifi[i].ssid==ssid;
        if (!ssid.empty() && password.empty() && !known && !(doc["wifi"]["open"] | false)) {
            error="初めて接続するWi-Fiのパスワードを入力してください。不要な場合はパスワードなしを選んでください。";
            return false;
        }
        if (doc["wifi"]["open"] | false) password.clear();
        if (password.empty() && !(doc["wifi"]["open"] | false)) {
            for (size_t i=0; i<current.wifi_count; ++i)
                if (current.wifi[i].ssid==ssid) password=current.wifi[i].password;
        }
        if (!ssid.empty()) {
            s.wifi_count=1; s.wifi[0]={ssid,password};
            if (!(doc["forget_others"] | false)) {
                for (size_t i=0; i<current.wifi_count && s.wifi_count<s.wifi.size(); ++i)
                    if (current.wifi[i].ssid!=ssid) s.wifi[s.wifi_count++]=current.wifi[i];
            }
        }
    }
    if (doc["backup"].is<JsonObject>()) {
        readBackup(doc["backup"],s);
        if (s.password.empty() && !(doc["backup"]["clear_password"] | false) &&
            s.url==current.url && s.username==current.username) s.password=current.password;
    }
    if (!validate(s,error,true)) return false;
    out=s;
    return true;
}
