#include "bf_wifi_link.h"

#include <string.h>
#include "sdkconfig.h"

#if !defined(CONFIG_BF_WIFI_LINK_ENABLED)

esp_err_t bf_wifi_link_init(bf_wifi_link_receiver_t receiver)
{
    (void)receiver;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bf_wifi_link_send(const uint8_t *data, size_t length)
{
    (void)data;
    (void)length;
    return ESP_ERR_NOT_SUPPORTED;
}

bool bf_wifi_link_ready(void) { return false; }

#else

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#if defined(CONFIG_BF_RADIO_ENABLED)
#error "BF_WIFI_LINK_ENABLED and BF_RADIO_ENABLED are mutually exclusive"
#endif

static const char *TAG = "wifi_link";
static const char *DISCOVER = "BF-RC-DISCOVER";
static const char *DISCOVER_REPLY = "BF-RC-HERE";

#define RX_TASK_STACK 4096
#define RX_TASK_PRIO 5
#define RX_TIMEOUT_US 20000
#define TX_QUEUE_LENGTH 8
#define TX_MAX_AGE_US 20000

typedef struct {
    struct sockaddr_in peer;
    int64_t queued_us;
    uint8_t data[BF_WIFI_LINK_FRAME_SIZE];
} tx_message_t;

static bf_wifi_link_receiver_t s_receiver;
static int s_socket = -1;
static TaskHandle_t s_task;
static QueueHandle_t s_tx_queue;
static portMUX_TYPE s_peer_lock = portMUX_INITIALIZER_UNLOCKED;
static struct sockaddr_in s_peer;
static bool s_peer_valid;
static bool s_ready;

#if defined(CONFIG_BF_WIFI_LINK_MODE_STA)
static void link_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ready = false;
        ESP_LOGW(TAG, "lost \"%s\", reconnecting", CONFIG_BF_WIFI_STA_SSID);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "joined \"%s\": " IPSTR ", UDP %d", CONFIG_BF_WIFI_STA_SSID,
                 IP2STR(&event->ip_info.ip), CONFIG_BF_WIFI_LINK_PORT);
        s_ready = true;
    }
}
#endif

static void transmit_task(void *argument)
{
    (void)argument;
    tx_message_t message;
    while (true) {
        if (xQueueReceive(s_tx_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_ready || esp_timer_get_time() - message.queued_us >= TX_MAX_AGE_US) {
            continue;
        }
        sendto(s_socket, message.data, sizeof(message.data), MSG_DONTWAIT,
               (struct sockaddr *)&message.peer, sizeof(message.peer));
    }
}

static void receive_task(void *argument)
{
    (void)argument;
    uint8_t buffer[128];
    while (true) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s_socket, &readable);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = RX_TIMEOUT_US };
        int selected = select(s_socket + 1, &readable, NULL, NULL, &timeout);
        if (selected <= 0) {
            continue;
        }
        struct sockaddr_in source;
        socklen_t length = sizeof(source);
        int count = recvfrom(s_socket, buffer, sizeof(buffer), 0,
                             (struct sockaddr *)&source, &length);
        if (count <= 0) {
            continue;
        }
        if (count == (int)strlen(DISCOVER) &&
            memcmp(buffer, DISCOVER, strlen(DISCOVER)) == 0) {
            sendto(s_socket, DISCOVER_REPLY, strlen(DISCOVER_REPLY), 0,
                   (struct sockaddr *)&source, length);
            continue;
        }
        if (count != (int)BF_WIFI_LINK_FRAME_SIZE) {
            continue;
        }
        taskENTER_CRITICAL(&s_peer_lock);
        s_peer = source;
        s_peer_valid = true;
        taskEXIT_CRITICAL(&s_peer_lock);
        if (s_receiver) {
            s_receiver(buffer, (size_t)count, esp_timer_get_time());
        }
    }
}

esp_err_t bf_wifi_link_init(bf_wifi_link_receiver_t receiver)
{
    if (!receiver) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ready) {
        return ESP_OK;
    }
#if defined(CONFIG_BF_WIFI_LINK_MODE_STA)
    const char *ssid = CONFIG_BF_WIFI_STA_SSID;
    const char *password = CONFIG_BF_WIFI_STA_PASSWORD;
#else
    const char *ssid = CONFIG_BF_WIFI_AP_SSID;
    const char *password = CONFIG_BF_WIFI_AP_PASSWORD;
#endif
    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > 32) {
        ESP_LOGE(TAG, "SSID must be at most 32 bytes");
        return ESP_ERR_INVALID_ARG;
    }
    size_t passphrase = strlen(password);
    if (passphrase != 0 && (passphrase < 8 || passphrase > 63)) {
        ESP_LOGE(TAG, "WPA2 passphrase must be 8..63 characters");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
    ESP_ERROR_CHECK(esp_netif_init());
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
#if defined(CONFIG_BF_WIFI_LINK_MODE_STA)
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_ERR_NO_MEM;
    }
#else
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (!netif) {
        return ESP_ERR_NO_MEM;
    }
#endif

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t config = { 0 };
#if defined(CONFIG_BF_WIFI_LINK_MODE_STA)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    strncpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strncpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = passphrase == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, link_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, link_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "joining \"%s\", UDP %d", ssid, CONFIG_BF_WIFI_LINK_PORT);
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    strncpy((char *)config.ap.ssid, ssid, sizeof(config.ap.ssid));
    config.ap.ssid_len = (uint8_t)strlen(ssid);
    config.ap.channel = CONFIG_BF_WIFI_AP_CHANNEL;
    config.ap.max_connection = 4;
    if (passphrase == 0) {
        config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "AP is open; use a WPA2 passphrase outside the bench");
    } else {
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strncpy((char *)config.ap.password, password, sizeof(config.ap.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
#endif

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0) {
        return ESP_FAIL;
    }
    int reuse = 1;
    setsockopt(s_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_BF_WIFI_LINK_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_socket, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(s_socket);
        s_socket = -1;
        return ESP_FAIL;
    }

    s_receiver = receiver;
    s_tx_queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(tx_message_t));
    if (!s_tx_queue) {
        close(s_socket);
        s_socket = -1;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(receive_task, "wifi_rc", RX_TASK_STACK, NULL, RX_TASK_PRIO,
                    &s_task) != pdPASS) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        close(s_socket);
        s_socket = -1;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(transmit_task, "wifi_tx", RX_TASK_STACK, NULL, RX_TASK_PRIO,
                    NULL) != pdPASS) {
        vTaskDelete(s_task);
        s_task = NULL;
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        close(s_socket);
        s_socket = -1;
        return ESP_ERR_NO_MEM;
    }

#if !defined(CONFIG_BF_WIFI_LINK_MODE_STA)
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "AP \"%s\" address " IPSTR ", UDP %d",
                 ssid, IP2STR(&ip.ip), CONFIG_BF_WIFI_LINK_PORT);
    } else {
        ESP_LOGI(TAG, "AP \"%s\", UDP %d", ssid, CONFIG_BF_WIFI_LINK_PORT);
    }
    s_ready = true;
#endif
    return ESP_OK;
}

esp_err_t bf_wifi_link_send(const uint8_t *data, size_t length)
{
    if (!s_ready || !s_tx_queue || !data || length != BF_WIFI_LINK_FRAME_SIZE) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_message_t message = { .queued_us = esp_timer_get_time() };
    taskENTER_CRITICAL(&s_peer_lock);
    bool valid = s_peer_valid;
    message.peer = s_peer;
    taskEXIT_CRITICAL(&s_peer_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(message.data, data, length);
    return xQueueSend(s_tx_queue, &message, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

bool bf_wifi_link_ready(void) { return s_ready; }

#endif
