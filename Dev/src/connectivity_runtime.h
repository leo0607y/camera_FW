#pragma once
#include "network_settings.h"
#include <cstdint>
#include <string>

// Wi-Fiとバックアップ共通のリトライ間隔
class RetrySchedule {
public:
    bool ready(uint32_t now) const { return !waiting || uint32_t(now-started)>=wait; }
    void fail(uint32_t now) { started=now; wait=next; waiting=true; next=next<30000 ? next*2 : 60000; }
    void reset() { waiting=false; next=5000; }
private:
    uint32_t started=0, wait=0, next=5000;
    bool waiting=false;
};
