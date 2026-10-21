#include "backup_transport.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/sha256.h>
#include <cstring>
#include <ctime>

std::string backupDigest(const uint8_t *data,size_t length) {
    uint8_t digest[32];
    if (mbedtls_sha256(data,length,digest,0)!=0) return {};
    static const char hex[]="0123456789abcdef";
    std::string result(64,'0');
    for (size_t i=0;i<32;++i) { result[2*i]=hex[digest[i]>>4]; result[2*i+1]=hex[digest[i]&15]; }
    return result;
}
std::string backupDestinationId(const NetworkSettings &s) {
    const std::string value=s.provider+"\n"+s.url+"\n"+s.username;
    return backupDigest(reinterpret_cast<const uint8_t *>(value.data()),value.size());
}

namespace {
struct Verification { const uint8_t *data; size_t length; size_t offset=0; bool matches=true; bool finished=false; };
esp_err_t verifyEvent(esp_http_client_event_t *event) {
    auto &v=*static_cast<Verification *>(event->user_data);
    if (event->event_id==HTTP_EVENT_ON_DATA) {
        const size_t length=static_cast<size_t>(event->data_len);
        if (length>v.length-v.offset || std::memcmp(v.data+v.offset,event->data,length)!=0) {
            v.matches=false;
            return ESP_FAIL;
        }
        v.offset+=length;
    }
    if (event->event_id==HTTP_EVENT_ON_FINISH) v.finished=true;
    return ESP_OK;
}
}

bool uploadBackup(const NetworkSettings &s,const uint8_t *data,size_t length,
                  const std::string &digest,std::string &error) {
    if (s.provider!="webdav" || s.url.compare(0,8,"https://")!=0 || !data || !length || digest.size()!=64) {
        error="バックアップ設定または画像が不正です。"; return false;
    }
    if (std::time(nullptr)<1700000000) { error="HTTPSのための時刻同期を待っています。"; return false; }
    const std::string url=s.url+"/"+digest+".jpg";
    esp_http_client_config_t config={};
    config.url=url.c_str();
    config.timeout_ms=5000;
    config.disable_auto_redirect=true;
    config.keep_alive_enable=false;
    config.username=s.username.empty() ? nullptr : s.username.c_str();
    config.password=s.password.empty() ? nullptr : s.password.c_str();
    config.auth_type=s.username.empty() ? HTTP_AUTH_TYPE_NONE : HTTP_AUTH_TYPE_BASIC;
    if (s.ca_pem.empty()) config.crt_bundle_attach=esp_crt_bundle_attach;
    else config.cert_pem=s.ca_pem.c_str();
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if (!client) { error="HTTPSの準備に失敗しました。"; return false; }
    esp_http_client_set_method(client,HTTP_METHOD_PUT);
    esp_http_client_set_header(client,"Content-Type","image/jpeg");
    esp_http_client_set_post_field(client,reinterpret_cast<const char *>(data),static_cast<int>(length));
    const esp_err_t sent=esp_http_client_perform(client);
    const int put_status=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (sent!=ESP_OK || (put_status!=200 && put_status!=201 && put_status!=204)) {
        error="送信に失敗しました（HTTP "+std::to_string(put_status)+"）。接続先と認証情報を確認してください。";
        return false;
    }
    Verification verification{data,length};
    config.event_handler=verifyEvent;
    config.user_data=&verification;
    client=esp_http_client_init(&config);
    if (!client) { error="保存先の照合を開始できませんでした。"; return false; }
    esp_http_client_set_method(client,HTTP_METHOD_GET);
    esp_http_client_set_header(client,"Cache-Control","no-cache");
    const esp_err_t received=esp_http_client_perform(client);
    const int get_status=esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (received!=ESP_OK || get_status!=200 || !verification.matches || !verification.finished || verification.offset!=length) {
        error="保存先のデータを照合できませんでした。後で再送します。"; return false;
    }
    error.clear();
    return true;
}
