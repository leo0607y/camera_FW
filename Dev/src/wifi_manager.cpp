#include "wifi_manager.h"
#include "network_settings.h"
#include "connectivity_runtime.h"
#include "backup_store.h"
#include "backup_transport.h"
#include "sd_card.h"
#include "led_control.h"
#include "camera_control.h"
#include "setup_page.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ctime>

namespace {
NetworkSettings settings;
Preferences preferences;
WebServer server(80);
RetrySchedule wifi_retry, backup_retry;
bool storage_ready=false, setup_active=false, setup_offered=false, close_requested=false, changed=false;
bool connecting=false, was_connected=false;
uint32_t setup_started=0, connect_started=0, offline_since=0, last_backup_step=0, last_scan=0;
uint32_t scan_generation=0, scan_revision=0, uploaded_count=0;
uint32_t deferred_count=0;
size_t profile=0;
bool scan_finished=false;
bool manual_backup=false;
std::string csrf, cursor, backup_message="自動バックアップは無効です。", ap_name;

bool setupRequest(bool modifying=false) {
    if (!setup_active || server.client().localIP()!=WiFi.softAPIP()) {
        server.send(403,"text/plain","Connect to the camera setup Wi-Fi"); return false;
    }
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("X-Content-Type-Options","nosniff");
    if (modifying && server.header("X-Setup-Token")!=csrf.c_str()) {
        server.send(403,"text/plain","Invalid setup session"); return false;
    }
    return true;
}
void jsonResponse(int code,const std::string &json) { server.send(code,"application/json; charset=utf-8",json.c_str()); }
void messageResponse(int code,const std::string &message) {
    JsonDocument doc; doc["message"]=message;
    std::string json; serializeJson(doc,json); jsonResponse(code,json);
}
void startSetup() {
    if (setup_active) return;
    char token[33];
    snprintf(token,sizeof(token),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),
             (unsigned long)esp_random(),(unsigned long)esp_random());
    csrf=token;
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(ap_name.c_str(),settings.setup_password.c_str())) {
        Serial.println("[Setup] Could not start access point"); return;
    }
    setup_active=true; setup_offered=true; setup_started=millis();
    Serial.printf("[Setup] SSID: %s, open http://192.168.4.1 (10 minutes)\n",ap_name.c_str());
}
void closeSetup() {
    setup_active=false; close_requested=false;
    WiFi.softAPdisconnect(true);
    Serial.println("[Setup] Access point closed");
}
void configureRoutes() {
    const char *headers[]={"X-Setup-Token"};
    server.collectHeaders(headers,1);
    server.on("/",HTTP_GET,[] {
        if (!setupRequest()) return;
        server.sendHeader("Content-Security-Policy","default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
        server.send(200,"text/html; charset=utf-8",SETUP_PAGE);
    });
    server.on("/api/settings",HTTP_GET,[] {
        if (!setupRequest()) return;
        JsonDocument doc;
        deserializeJson(doc,encodeSettings(settings,false));
        doc["csrf"]=csrf;
        doc["ap_name"]=ap_name;
        std::string json; serializeJson(doc,json); jsonResponse(200,json);
    });
    server.on("/api/settings",HTTP_POST,[] {
        if (!setupRequest(true)) return;
        const String body=server.arg("plain");
        NetworkSettings next; std::string error;
        if (!applySettingsForm(std::string(body.c_str(),body.length()),settings,next,error)) {
            messageResponse(400,error); return;
        }
        const std::string encoded=encodeSettings(next,true);
        if (!storage_ready || encoded.size()>8192 ||
            preferences.putBytes("settings-bin",encoded.data(),encoded.size())!=encoded.size()) {
            messageResponse(500,"本体に設定を保存できませんでした。設定は変更していません。"); return;
        }
        settings=next; changed=true;
        messageResponse(200,"設定を保存しました。接続状況を確認してから「設定を終了」を押してください。設定用Wi-Fiの新しいパスワードは次回から使います。");
    });
    server.on("/api/scan",HTTP_GET,[] {
        if (!setupRequest()) return;
        JsonDocument doc;
        const int count=WiFi.scanComplete();
        if (count==WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(true);
            doc["scanning"]=true;
        } else if (count==WIFI_SCAN_RUNNING) doc["scanning"]=true;
        else {
            doc["scanning"]=false;
            auto networks=doc["networks"].to<JsonArray>();
            for (int i=0;i<count && i<30;++i) {
                auto n=networks.add<JsonObject>();
                n["ssid"]=WiFi.SSID(i).c_str(); n["rssi"]=WiFi.RSSI(i);
            }
        }
        std::string json; serializeJson(doc,json); jsonResponse(200,json);
    });
    server.on("/api/rescan",HTTP_POST,[] {
        if (!setupRequest(true)) return;
        if (WiFi.scanComplete()!=WIFI_SCAN_RUNNING) { WiFi.scanDelete(); WiFi.scanNetworks(true); }
        messageResponse(200,"Wi-Fiを検索しています。");
    });
    server.on("/api/status",HTTP_GET,[] {
        if (!setupRequest()) return;
        JsonDocument doc;
        doc["connected"]=WiFi.status()==WL_CONNECTED;
        doc["ssid"]=WiFi.status()==WL_CONNECTED ? WiFi.SSID().c_str() : "";
        doc["backup_message"]=backup_message;
        doc["uploaded"]=uploaded_count;
        doc["sd_ready"]=isSDCardMounted();
        doc["camera_error"]=captureState()==CaptureState::Error;
        const uint32_t elapsed=uint32_t(millis()-setup_started)/1000;
        doc["setup_seconds"]=elapsed>=600 ? 0 : 600-elapsed;
        std::string json; serializeJson(doc,json); jsonResponse(200,json);
    });
    server.on("/api/finish",HTTP_POST,[] {
        if (!setupRequest(true)) return;
        messageResponse(200,"設定用Wi-Fiを終了します。カメラは単体で撮影・転送を続けます。");
        close_requested=true;
    });
    server.on("/api/test",HTTP_POST,[] {
        if (!setupRequest(true)) return;
        if (!settings.backup_enabled || WiFi.status()!=WL_CONNECTED) {
            messageResponse(409,"設定を保存し、自動バックアップを有効にしてWi-Fi接続を確認してください。"); return;
        }
        manual_backup=true; scan_finished=false; cursor.clear(); deferred_count=0; backup_retry.reset();
        messageResponse(202,"未転送の写真を1件送信して照合します。結果はバックアップの状態に表示します。");
    });
    server.onNotFound([] { if (setupRequest()) server.send(404,"text/plain","Not found"); });
    server.begin();
}
void updateWiFi() {
    const uint32_t now=millis();
    const bool connected=WiFi.status()==WL_CONNECTED;
    setWiFiStatusLED(connected);
    if (connected) {
        connecting=false; wifi_retry.reset();
        if (!was_connected) {
            configTime(0,0,settings.ntp_server.c_str());
            setup_offered=setup_active;
            Serial.println("[WiFi] Connected; requesting clock synchronization");
        }
        was_connected=true;
        return;
    }
    if (was_connected) { offline_since=now; was_connected=false; setup_offered=setup_active; profile=0; wifi_retry.reset(); }
    if (!setup_offered && uint32_t(now-offline_since)>=60000) startSetup();
    if (!settings.wifi_count) return;
    if (connecting) {
        if (uint32_t(now-connect_started)<20000) return;
        WiFi.disconnect(false,false,0);
        connecting=false;
        profile=(profile+1)%settings.wifi_count;
        wifi_retry.fail(now);
        return;
    }
    if (!wifi_retry.ready(now) || WiFi.scanComplete()==WIFI_SCAN_RUNNING) return;
    const auto &w=settings.wifi[profile];
    WiFi.begin(w.ssid.c_str(),w.password.empty() ? nullptr : w.password.c_str());
    connecting=true; connect_started=now;
    Serial.printf("[WiFi] Trying saved network %u\n",unsigned(profile+1));
}
void updateBackup() {
    const uint32_t now=millis();
    if (!settings.backup_enabled) { backup_message="自動バックアップは無効です。"; return; }
    if (setup_active && !manual_backup) return;
    if (WiFi.status()!=WL_CONNECTED) { backup_message="Wi-Fi接続待ち。写真はSDカードに残っています。"; return; }
    if (!isSDCardMounted()) { backup_message="SDカードの復旧を待っています。"; return; }
    if (!backup_retry.ready(now) || uint32_t(now-last_backup_step)<250) return;
    last_backup_step=now;
    const uint32_t generation=sdGeneration();
    if (generation!=scan_generation) { cursor.clear(); deferred_count=0; scan_finished=false; scan_generation=generation; scan_revision=sdRevision(); }
    if (scan_finished) {
        if (scan_revision==sdRevision() && uint32_t(now-last_scan)<60000) return;
        cursor.clear(); deferred_count=0; scan_finished=false; scan_revision=sdRevision();
    }
    PhotoSnapshot photo;
    const SnapshotResult result=nextBackupPhoto(cursor,photo);
    if (result==SnapshotResult::End) {
        scan_finished=true; manual_backup=false; last_scan=now;
        backup_message=deferred_count ? "未転送の画像が"+std::to_string(deferred_count)+"件あります。読み出しエラーまたは2MB超の画像を確認してください。" :
            "今回の走査が完了しました。未転送の写真がなければ送信は行いません。"; return;
    }
    if (result==SnapshotResult::Unavailable) {
        backup_message="SDの読み出しまたは作業メモリーを確保できません。後で再試行します。";
        manual_backup=false;
        backup_retry.fail(now); return;
    }
    if (result==SnapshotResult::Skipped) {
        ++deferred_count;
        Serial.printf("[Backup] Deferred unreadable/oversized photo: %s\n",photo.source.c_str());
        backup_message="読めない画像、または2MBを超える画像があります。SD上の写真を確認してください。";
        cursor=photo.source; return;
    }
    const std::string digest=backupDigest(photo.data.get(),photo.size);
    const std::string destination=backupDestinationId(settings);
    if (digest.empty() || destination.empty()) { backup_retry.fail(now); return; }
    if (hasBackupReceipt(destination,digest)) { cursor=photo.source; return; }
    backup_message="写真を送信し、保存先の内容を照合しています。";
    std::string error;
    if (!uploadBackup(settings,photo.data.get(),photo.size,digest,error)) {
        manual_backup=false;
        backup_message=error; backup_retry.fail(millis());
        Serial.printf("[Backup] Retry scheduled: %s\n",error.c_str()); return;
    }
    if (!writeBackupReceipt(photo,destination,digest)) {
        manual_backup=false;
        backup_message="送信済みですがSDに完了を記録できません。重複しない名前で再確認します。";
        backup_retry.fail(millis()); return;
    }
    manual_backup=false; backup_retry.reset(); cursor=photo.source; ++uploaded_count;
    backup_message="バックアップと保存先の照合が完了しました。";
    Serial.printf("[Backup] Verified: %s\n",photo.source.c_str());
}
void networkTask(void *argument) {
    const bool requested=argument!=nullptr;
    storage_ready=preferences.begin("camera-net",false);
    if (storage_ready) {
        std::string encoded;
        const size_t length=preferences.getBytesLength("settings-bin");
        if (length>0 && length<=8192) {
            encoded.resize(length);
            if (preferences.getBytes("settings-bin",&encoded[0],length)!=length) encoded.clear();
        } else if (length==0) {
            // 古い開発版の文字列形式も受け付ける
            encoded=preferences.getString("settings","").c_str();
        }
        std::string error;
        if (encoded.length() && !decodeSettings(encoded,settings,error))
            Serial.println("[Setup] Stored settings invalid; opening setup mode");
    }
    char name[32]; snprintf(name,sizeof(name),"Camera-%06lx",(unsigned long)(ESP.getEfuseMac()&0xffffff));
    ap_name=name;
    WiFi.persistent(false); WiFi.setAutoReconnect(false); WiFi.mode(WIFI_STA);
    offline_since=millis();
    configureRoutes();
    if (requested || !settings.wifi_count) startSetup();
    for (;;) {
        if (setup_active) server.handleClient();
        if (changed) {
            changed=false; WiFi.disconnect(false,false,0); connecting=false; was_connected=false;
            profile=0; wifi_retry.reset(); backup_retry.reset(); cursor.clear(); deferred_count=0; scan_finished=false;
            scan_revision=sdRevision(); offline_since=millis();
            manual_backup=false;
            backup_message="設定を保存しました。設定終了後に自動転送します。";
        }
        if (setup_active && (close_requested || uint32_t(millis()-setup_started)>=600000)) closeSetup();
        updateWiFi();
        updateBackup();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}
void beginConnectivity(bool request_setup) {
    // 撮影バッファは持たない。SDをロックするのはPSRAMへのコピーと記録の書き込みだけ（HTTP中は持たない）
    if (xTaskCreate(networkTask,"camera-network",12288,request_setup ? reinterpret_cast<void *>(1) : nullptr,1,nullptr)!=pdPASS)
        Serial.println("[Network] Could not start worker; local photography remains available");
}
