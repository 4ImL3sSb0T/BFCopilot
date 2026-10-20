#include "bsp_wifi.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_BF_WIFI_ENABLED

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

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

static exit_code_t bsp_wifi_status(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return EXIT_OK;
    case ESP_ERR_INVALID_ARG:
        return EXIT_INVALID_PARAM;
    case ESP_ERR_INVALID_STATE:
        return EXIT_NOT_INITIALIZED;
    case ESP_ERR_NO_MEM:
        return EXIT_NO_MEMORY;
    case ESP_ERR_NOT_FOUND:
        return EXIT_DOES_NOT_EXIST;
    case ESP_ERR_NOT_SUPPORTED:
        return EXIT_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT:
        return EXIT_TIMEOUT;
    default:
        return EXIT_HW_FAILURE;
    }
}

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

exit_code_t bsp_wifi_init(void) {
    if (s_ready) {
        return EXIT_ALREADY_INITIALIZED;
    }

    const char *ssid = CONFIG_BF_WIFI_SSID;
    const char *password = CONFIG_BF_WIFI_PASSWORD;
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64 ||
        (password_length > 0 && password_length < 8)) {
        ESP_LOGE(TAG, "invalid credentials: SSID must be 1-32 bytes, password empty or 8-64 bytes");
        return EXIT_INVALID_PARAM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return bsp_wifi_status(err);
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return bsp_wifi_status(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return bsp_wifi_status(err);
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "failed to create default Wi-Fi STA netif");
        return EXIT_HW_FAILURE;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return bsp_wifi_status(err);
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &bsp_wifi_event_handler, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &bsp_wifi_ip_handler, NULL, NULL);
    }
    if (err != ESP_OK) {
        return bsp_wifi_status(err);
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
        return bsp_wifi_status(err);
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return EXIT_NO_MEMORY;
    }
    s_ready = true;
    ESP_LOGI(TAG, "station configured for \"%s\" (radio not started yet)", ssid);
    return EXIT_OK;
}

exit_code_t bsp_wifi_connect(u32 timeout_ms) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (bsp_wifi_is_connected()) {
        return EXIT_OK;
    }

    xEventGroupClearBits(s_events, BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT);
    s_should_connect = true;

    esp_err_t err = ESP_OK;
    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return bsp_wifi_status(err);
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
        return EXIT_OK;
    }
    return (bits & BSP_WIFI_FAIL_BIT) ? EXIT_FAIL : EXIT_TIMEOUT;
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

exit_code_t bsp_wifi_get_status(bsp_wifi_status_t *status) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (status == NULL) {
        return EXIT_INVALID_PARAM;
    }
    portENTER_CRITICAL(&s_lock);
    status->connected = s_connected;
    memcpy(status->ip, s_ip, sizeof(status->ip));
    portEXIT_CRITICAL(&s_lock);
    return EXIT_OK;
}

#else  // !CONFIG_BF_WIFI_ENABLED

exit_code_t bsp_wifi_init(void) {
    return EXIT_NOT_SUPPORTED;
}

exit_code_t bsp_wifi_connect(u32 timeout_ms) {
    (void)timeout_ms;
    return EXIT_NOT_SUPPORTED;
}

void bsp_wifi_disconnect(void) {
}

bool bsp_wifi_is_connected(void) {
    return false;
}

exit_code_t bsp_wifi_get_status(bsp_wifi_status_t *status) {
    (void)status;
    return EXIT_NOT_SUPPORTED;
}

#endif
