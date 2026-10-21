#include "bf_espnow.h"

#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static uint8_t s_peer[6];
static bool s_ready;

#ifdef CONFIG_BF_RADIO_ENABLED
static bf_espnow_receiver_t s_receiver;
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
    if (s_receiver && length > 0) {
        s_receiver(data, (size_t)length, esp_timer_get_time());
    }
}
#endif

esp_err_t bf_espnow_init(bf_espnow_receiver_t receiver)
{
#ifndef CONFIG_BF_RADIO_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!receiver) return ESP_ERR_INVALID_ARG;
    if (s_ready) return ESP_OK;
    s_receiver = receiver;
    uint8_t pmk[16];
    esp_now_peer_info_t peer = { .channel = CONFIG_BF_RADIO_CHANNEL,
                                 .ifidx = WIFI_IF_STA, .encrypt = true };
    if (!from_hex(CONFIG_BF_RADIO_PEER, s_peer, 6) || (s_peer[0] & 1) ||
        !from_hex(CONFIG_BF_RADIO_PMK, pmk, sizeof(pmk)) ||
        !from_hex(CONFIG_BF_RADIO_LMK, peer.lmk, sizeof(peer.lmk))) {
        return ESP_ERR_INVALID_ARG;
    }
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

esp_err_t bf_espnow_send(const uint8_t *data, size_t length)
{
    if (!s_ready || !data || !length) return ESP_ERR_INVALID_STATE;
    return esp_now_send(s_peer, data, length);
}
