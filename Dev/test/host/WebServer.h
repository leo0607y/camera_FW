#pragma once
#include "Arduino.h"
#include <functional>
#include <map>
constexpr int HTTP_GET=0,HTTP_POST=1;
inline int mock_local_ip=1,mock_response_code=0;
inline std::string mock_response,mock_body;
inline std::map<std::string,std::string> mock_request_headers,mock_response_headers;
inline std::map<std::pair<std::string,int>,std::function<void()>> mock_routes;
struct MockClient { void write(uint8_t *, size_t) {} int localIP() { return mock_local_ip; } };
class WebServer {
public:
    explicit WebServer(int=80) {}
    void send(int code, const char *, const String &body) { mock_response_code=code;mock_response=body.c_str(); }
    void sendHeader(const char *name, const String &value) { mock_response_headers[name]=value.c_str(); }
    MockClient client() { return {}; }
    void collectHeaders(const char **,size_t) {}
    String header(const char *name) { return mock_request_headers[name]; }
    String arg(const char *) { return mock_body; }
    void on(const char *path,int method,std::function<void()> fn) { mock_routes[{path,method}]=fn; }
    void onNotFound(std::function<void()>) {}
    void begin() {}
    void handleClient() {}
};
