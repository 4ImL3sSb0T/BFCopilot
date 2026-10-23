#include "bf_radio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#if defined(CONFIG_BF_RADIO_ENABLED) && defined(CONFIG_BF_WIFI_LINK_ENABLED)
#error "CONFIG_BF_RADIO_ENABLED and CONFIG_BF_WIFI_LINK_ENABLED are mutually exclusive"
#endif

#if defined(CONFIG_BF_WIFI_LINK_ENABLED)
#include "bf_wifi_link.h"
#elif defined(CONFIG_BF_RADIO_ENABLED)
#include "bf_espnow.h"
#endif

static QueueHandle_t s_queue;
static QueueHandle_t s_estop;
static uint32_t s_dropped;
static bool s_ready;

#if defined(CONFIG_BF_RADIO_ENABLED) || defined(CONFIG_BF_WIFI_LINK_ENABLED)
static void receive_frame(const uint8_t *data, size_t length, int64_t received_us)
{
    bf_radio_message_t message = { .received_us = received_us };
    if (!bf_frame_decode(data, length, &message.frame)) {
        __atomic_fetch_add(&s_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (message.frame.type == BF_ESTOP) {
        xQueueOverwrite(s_estop, &message);
    } else if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        __atomic_fetch_add(&s_dropped, 1, __ATOMIC_RELAXED);
    }
}
#endif

esp_err_t bf_radio_init(void)
{
#if !defined(CONFIG_BF_RADIO_ENABLED) && !defined(CONFIG_BF_WIFI_LINK_ENABLED)
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_ready) return ESP_OK;
    s_queue = xQueueCreate(8, sizeof(bf_radio_message_t));
    s_estop = xQueueCreate(1, sizeof(bf_radio_message_t));
    esp_err_t result = ESP_ERR_NO_MEM;
    if (s_queue && s_estop) {
#if defined(CONFIG_BF_WIFI_LINK_ENABLED)
        result = bf_wifi_link_init(receive_frame);
#else
        result = bf_espnow_init(receive_frame);
#endif
    }
    if (result != ESP_OK) {
        if (s_queue) vQueueDelete(s_queue);
        if (s_estop) vQueueDelete(s_estop);
        s_queue = NULL;
        s_estop = NULL;
        return result;
    }
    s_ready = true;
    return ESP_OK;
#endif
}

esp_err_t bf_radio_send(const bf_frame_t *frame)
{
    if (!s_ready || !frame) return ESP_ERR_INVALID_STATE;
    uint8_t data[BF_FRAME_SIZE];
    bf_frame_encode(frame, data);
#if defined(CONFIG_BF_WIFI_LINK_ENABLED)
    return bf_wifi_link_send(data, sizeof(data));
#elif defined(CONFIG_BF_RADIO_ENABLED)
    return bf_espnow_send(data, sizeof(data));
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool bf_radio_receive(bf_radio_message_t *message)
{
    return s_queue && xQueueReceive(s_queue, message, 0) == pdTRUE;
}

bool bf_radio_take_estop(void)
{
    bf_radio_message_t message;
    return s_estop && xQueueReceive(s_estop, &message, 0) == pdTRUE;
}

uint32_t bf_radio_dropped(void)
{
    return __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
}
