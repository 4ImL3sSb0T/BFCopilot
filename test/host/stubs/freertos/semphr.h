#pragma once
typedef int StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, int wait) { (void)mutex; (void)wait; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; return 1; }
