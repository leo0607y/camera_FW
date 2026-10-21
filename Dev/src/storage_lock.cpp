#include "storage_lock.h"
SemaphoreHandle_t storageMutex() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex=xSemaphoreCreateRecursiveMutexStatic(&storage);
    return mutex;
}
