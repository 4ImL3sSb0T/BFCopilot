#include "bsp_wifi.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_BF_WIFI_ENABLED

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "bsp_params.h"

#define BSP_WIFI_CONNECTED_BIT (1u << 0)
#define BSP_WIFI_FAIL_BIT      (1u << 1)

static const char *TAG = "bsp_wifi";

static EventGroupHandle_t s_events;
static bool s_ready;
static bool s_started;
static volatile bool s_should_connect;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_connected;
static char s_ip[16];

static void bsp_wifi_mark_connected(const char *ip) {
    portENTER_CRITICAL(&s_lock);
    s_connected = true;
    memcpy(s_ip, ip, sizeof(s_ip));
    portEXIT_CRITICAL(&s_lock);
}

static void bsp_wifi_mark_disconnected(void) {
    portENTER_CRITICAL(&s_lock);
    s_connected = false;
    s_ip[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

static void bsp_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        if (s_should_connect) {
            esp_wifi_connect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *info = data;
        bsp_wifi_mark_disconnected();
        xEventGroupClearBits(s_events, BSP_WIFI_CONNECTED_BIT);
        if (s_should_connect) {
            // 保留重连：台架链路断线后希望自己回来，失败原因只看日志。
            ESP_LOGW(TAG, "link down (reason %d), reconnecting", info->reason);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BSP_WIFI_FAIL_BIT);
        }
    }
}

static void bsp_wifi_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;

    const ip_event_got_ip_t *event = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    bsp_wifi_mark_connected(ip);
    xEventGroupClearBits(s_events, BSP_WIFI_FAIL_BIT);
    xEventGroupSetBits(s_events, BSP_WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG, "connected to \"%s\", ip %s", CONFIG_BF_WIFI_SSID, ip);
}

esp_err_t bsp_wifi_init(void) {
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *ssid = CONFIG_BF_WIFI_SSID;
    const char *password = CONFIG_BF_WIFI_PASSWORD;
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64 ||
        (password_length > 0 && password_length < 8)) {
        ESP_LOGE(TAG, "invalid credentials: SSID must be 1-32 bytes, password empty or 8-64 bytes");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = bsp_nvs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    bool wifi_initialized = false;
    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (netif == NULL) {
        ESP_LOGE(TAG, "failed to create default Wi-Fi STA netif");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        goto fail;
    }
    wifi_initialized = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &bsp_wifi_event_handler, NULL, &wifi_handler);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &bsp_wifi_ip_handler, NULL, &ip_handler);
    }
    if (err != ESP_OK) {
        goto fail;
    }

    // 凭据来自 Kconfig，只在运行时使用；不写进 NVS，避免把口令留在 flash 里。
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }

    wifi_config_t station_config = {0};
    memcpy(station_config.sta.ssid, ssid, ssid_length);
    memcpy(station_config.sta.password, password, password_length);
    station_config.sta.threshold.authmode =
        password_length > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    station_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi configuration failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_ready = true;
    ESP_LOGI(TAG, "station configured for \"%s\" (radio not started yet)", ssid);
    return ESP_OK;

fail:
    if (ip_handler != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    }
    if (wifi_handler != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    }
    if (wifi_initialized) {
        esp_wifi_deinit();
    }
    if (netif != NULL) {
        esp_netif_destroy_default_wifi(netif);
    }
    vEventGroupDelete(s_events);
    s_events = NULL;
    return err;
}

esp_err_t bsp_wifi_connect(u32 timeout_ms) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bsp_wifi_is_connected()) {
        return ESP_OK;
    }

    xEventGroupClearBits(s_events, BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT);
    s_should_connect = true;

    esp_err_t err = ESP_OK;
    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return err;
        }
        s_started = true;
        // STA_START 事件里会发起第一次连接
    } else {
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            // 已在连接过程中时驱动会返回状态错误，继续等事件即可
            ESP_LOGD(TAG, "esp_wifi_connect returned %s", esp_err_to_name(err));
        }
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_events, BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BSP_WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return (bits & BSP_WIFI_FAIL_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void bsp_wifi_disconnect(void) {
    if (!s_ready) {
        return;
    }
    s_should_connect = false;
    esp_wifi_disconnect();
}

bool bsp_wifi_is_connected(void) {
    if (!s_ready) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    const bool connected = s_connected;
    portEXIT_CRITICAL(&s_lock);
    return connected;
}

esp_err_t bsp_wifi_get_status(bsp_wifi_status_t *status) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    status->connected = s_connected;
    memcpy(status->ip, s_ip, sizeof(status->ip));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

#else  // !CONFIG_BF_WIFI_ENABLED

esp_err_t bsp_wifi_init(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_wifi_connect(u32 timeout_ms) {
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_wifi_disconnect(void) {
}

bool bsp_wifi_is_connected(void) {
    return false;
}

esp_err_t bsp_wifi_get_status(bsp_wifi_status_t *status) {
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
