#pragma once
typedef int *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int storage; return &storage; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, int wait) { (void)mutex; (void)wait; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; return 1; }
