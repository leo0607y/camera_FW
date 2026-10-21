#pragma once
#include "esp_camera.h"
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
constexpr int ESP_FAIL=-1;
constexpr int HTTP_EVENT_ON_DATA=1,HTTP_EVENT_ON_FINISH=2;
constexpr int HTTP_AUTH_TYPE_NONE=0,HTTP_AUTH_TYPE_BASIC=1;
constexpr int HTTP_METHOD_PUT=1,HTTP_METHOD_GET=2;
struct esp_http_client_event_t { int event_id; void *user_data; char *data; int data_len; };
struct esp_http_client_config_t {
    const char *url=nullptr,*username=nullptr,*password=nullptr,*cert_pem=nullptr;
    int timeout_ms=0,auth_type=0;
    bool disable_auto_redirect=false,keep_alive_enable=false;
    int (*crt_bundle_attach)(void *)=nullptr;
    int (*event_handler)(esp_http_client_event_t *)=nullptr;
    void *user_data=nullptr;
};
struct MockHTTP {
    esp_http_client_config_t config;
    int method=0,status=0;
    const char *body=nullptr;
    size_t size=0;
};
using esp_http_client_handle_t=MockHTTP *;
inline std::vector<uint8_t> mock_remote;
inline int mock_put_status=201,mock_get_status=200,mock_put_calls=0,mock_get_calls=0;
inline bool mock_http_ok=true,mock_http_reply_lost=false,mock_remote_corrupt=false,mock_remote_short=false;
inline std::function<void()> mock_on_http;
inline std::string mock_last_url;
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    assert(c->disable_auto_redirect);
    assert(c->cert_pem || c->crt_bundle_attach);
    assert(std::string(c->url).rfind("https://",0)==0);
    mock_last_url=c->url;
    return new MockHTTP{*c};
}
inline void esp_http_client_set_method(MockHTTP *c,int method) { c->method=method; }
inline void esp_http_client_set_header(MockHTTP *,const char *,const char *) {}
inline void esp_http_client_set_post_field(MockHTTP *c,const char *data,int size) { c->body=data;c->size=size; }
inline int esp_http_client_perform(MockHTTP *c) {
    if (mock_on_http) mock_on_http();
    if (!mock_http_ok) return ESP_FAIL;
    if (c->method==HTTP_METHOD_PUT) {
        ++mock_put_calls;c->status=mock_put_status;
        if (c->status==200 || c->status==201 || c->status==204) mock_remote.assign(c->body,c->body+c->size);
        return mock_http_reply_lost ? ESP_FAIL : ESP_OK;
    }
    ++mock_get_calls;c->status=mock_get_status;
    auto contents=mock_remote;
    if (mock_remote_corrupt && !contents.empty()) contents[contents.size()/2]^=0xff;
    if (mock_remote_short && !contents.empty()) contents.pop_back();
    for (size_t offset=0;offset<contents.size();) {
        const size_t size=std::min<size_t>(37,contents.size()-offset);
        esp_http_client_event_t e{HTTP_EVENT_ON_DATA,c->config.user_data,reinterpret_cast<char *>(contents.data()+offset),int(size)};
        if (c->config.event_handler(&e)!=ESP_OK) return ESP_FAIL;
        offset+=size;
    }
    esp_http_client_event_t e{HTTP_EVENT_ON_FINISH,c->config.user_data,nullptr,0};
    c->config.event_handler(&e);
    return ESP_OK;
}
inline int esp_http_client_get_status_code(MockHTTP *c) { return c->status; }
inline void esp_http_client_cleanup(MockHTTP *c) { delete c; }
