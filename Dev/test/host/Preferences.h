#pragma once
#include "Arduino.h"
#include <cstring>
inline std::string mock_saved_settings;
inline bool mock_preferences_ok=true;
class Preferences {
public:
    bool begin(const char *,bool) { return mock_preferences_ok; }
    String getString(const char *,const char *) { return mock_saved_settings; }
    size_t getBytesLength(const char *) { return mock_saved_settings.size(); }
    size_t getBytes(const char *,void *out,size_t length) {
        if (length!=mock_saved_settings.size()) return 0;
        std::memcpy(out,mock_saved_settings.data(),length);return length;
    }
    size_t putBytes(const char *,const void *data,size_t length) {
        if (!mock_preferences_ok) return 0;
        mock_saved_settings.assign(static_cast<const char *>(data),length);return length;
    }
    size_t putString(const char *,const char *value) {
        if (!mock_preferences_ok) return 0;
        mock_saved_settings=value;return mock_saved_settings.size();
    }
};
