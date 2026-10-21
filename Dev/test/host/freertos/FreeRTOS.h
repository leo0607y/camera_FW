#pragma once
#include <mutex>
using StaticSemaphore_t=std::recursive_mutex;
using SemaphoreHandle_t=std::recursive_mutex *;
constexpr unsigned portMAX_DELAY=0xffffffff;
