#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s) { return s; }
inline void xSemaphoreTakeRecursive(SemaphoreHandle_t s,unsigned) { s->lock(); }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t s) { s->unlock(); }
