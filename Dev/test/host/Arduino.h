#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <type_traits>
#include <functional>
#define PROGMEM

class String {
    std::string value;
public:
    String() = default;
    String(const char *s) : value(s) {}
    String(std::string s) : value(s) {}
    template<class T, typename = std::enable_if_t<std::is_integral_v<T>>>
    String(T n) : value(std::to_string(n)) {}
    const char *c_str() const { return value.c_str(); }
    size_t length() const { return value.length(); }
    bool operator!=(const char *other) const { return value!=other; }
    bool endsWith(const char *suffix) const {
        std::string s(suffix);
        return value.size() >= s.size() && value.compare(value.size()-s.size(), s.size(), s) == 0;
    }
    String &operator+=(const String &s) { value += s.value; return *this; }
    friend String operator+(const String &a, const String &b) { return a.value + b.value; }
};

constexpr int LOW=0, HIGH=1, OUTPUT=1, INPUT_PULLUP=2;
inline uint32_t mock_time=10000;
inline std::function<void()> mock_delay_hook;
inline int mock_pins[64] = {};
inline unsigned long millis() { return mock_time; }
inline void delay(unsigned long ms) { mock_time += ms; const auto hook=mock_delay_hook; if (hook) hook(); }
inline void delayMicroseconds(unsigned long) {}
inline void pinMode(int pin, int mode) { if (mode == INPUT_PULLUP) mock_pins[pin] = HIGH; }
inline void digitalWrite(int pin, int value) { mock_pins[pin] = value; }
inline int digitalRead(int pin) { return mock_pins[pin]; }
struct MockSerial {
    int messages=0;
    operator bool() const { return true; }
    void begin(uint32_t) {}
    void write(int) {}
    template<class... T> void printf(const char *, T...) { ++messages; }
    template<class T> void println(T) { ++messages; }
    template<class T> void print(T) { ++messages; }
};
inline MockSerial Serial;
struct MockESP { uint64_t getEfuseMac() { return 0x123456; } };
inline MockESP ESP;
inline void configTime(int,int,const char *) {}
