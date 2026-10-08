#include "bf_radio.h"

#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static QueueHandle_t s_queue;
static QueueHandle_t s_estop;
static uint8_t s_peer[6];
static uint32_t s_dropped;
static bool s_ready;

#ifdef CONFIG_BF_RADIO_ENABLED
static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool from_hex(const char *text, uint8_t *out, size_t length)
{
    if (strlen(text) != length * 2) return false;
    bool nonzero = false;
    for (size_t index = 0; index < length; index++) {
        int high = hex_digit(text[index * 2]);
        int low = hex_digit(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        out[index] = (uint8_t)(high * 16 + low);
        nonzero |= out[index] != 0;
    }
    return nonzero;
}

static void receive_cb(const esp_now_recv_info_t *info, const uint8_t *data, int length)
{
    if (!info || memcmp(info->src_addr, s_peer, sizeof(s_peer)) != 0) return;
    bf_radio_message_t message = { .received_us = esp_timer_get_time() };
    if (length != BF_FRAME_SIZE || !bf_frame_decode(data, (size_t)length, &message.frame)) {
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
#ifndef CONFIG_BF_RADIO_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint8_t pmk[16];
    esp_now_peer_info_t peer = { .channel = CONFIG_BF_RADIO_CHANNEL,
                                 .ifidx = WIFI_IF_STA, .encrypt = true };
    if (!from_hex(CONFIG_BF_RADIO_PEER, s_peer, 6) || (s_peer[0] & 1) ||
        !from_hex(CONFIG_BF_RADIO_PMK, pmk, sizeof(pmk)) ||
        !from_hex(CONFIG_BF_RADIO_LMK, peer.lmk, sizeof(peer.lmk))) {
        return ESP_ERR_INVALID_ARG;
    }
    s_queue = xQueueCreate(8, sizeof(bf_radio_message_t));
    s_estop = xQueueCreate(1, sizeof(bf_radio_message_t));
    if (!s_queue || !s_estop) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_BF_RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk(pmk));
    memcpy(peer.peer_addr, s_peer, sizeof(s_peer));
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(receive_cb));
    s_ready = true;
    return ESP_OK;
#endif
}

esp_err_t bf_radio_send(const bf_frame_t *frame)
{
    if (!s_ready || !frame) return ESP_ERR_INVALID_STATE;
    uint8_t data[BF_FRAME_SIZE];
    bf_frame_encode(frame, data);
    return esp_now_send(s_peer, data, sizeof(data));
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
