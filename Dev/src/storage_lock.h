#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

SemaphoreHandle_t storageMutex();
class StorageGuard {
public:
    StorageGuard() { xSemaphoreTakeRecursive(storageMutex(),portMAX_DELAY); }
    ~StorageGuard() { xSemaphoreGiveRecursive(storageMutex()); }
    StorageGuard(const StorageGuard &)=delete;
    StorageGuard &operator=(const StorageGuard &)=delete;
};
