#pragma once
#include "Arduino.h"
#include <functional>
#include <stdexcept>
constexpr int pdPASS=1;
inline void (*mock_task)(void *)=nullptr;
inline void *mock_task_argument=nullptr;
inline int mock_task_ticks=0,mock_task_limit=10;
inline std::function<void(int)> mock_task_hook;
inline unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
inline int xTaskCreate(void (*task)(void *),const char *,int,void *argument,int,void *) {
    mock_task=task;mock_task_argument=argument;return pdPASS;
}
struct MockTaskFinished {};
inline void vTaskDelay(unsigned ms) {
    mock_time+=ms;
    if (mock_task_hook) mock_task_hook(mock_task_ticks);
    if (++mock_task_ticks>=mock_task_limit) throw MockTaskFinished{};
}
