#include "bf_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

static StaticSemaphore_t s_storage;
static SemaphoreHandle_t s_mutex;

void bf_runtime_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_storage);
    ESP_ERROR_CHECK(s_mutex ? ESP_OK : ESP_ERR_NO_MEM);
}

void bf_runtime_lock(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

void bf_runtime_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}
