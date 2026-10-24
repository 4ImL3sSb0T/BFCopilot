#include "net_test.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "bsp_net.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BF_NET_TEST_ENABLED

#define APP_NET_TEST_TCP_ENDPOINT 0
#define APP_NET_TEST_UDP_ENDPOINT 1
#define APP_NET_TEST_CLIENT_ENDPOINT 2
#define APP_NET_TEST_TASK_STACK 6144
#define APP_NET_TEST_TASK_PRIORITY 5
#define APP_NET_TEST_TCP_READ_SIZE 256
#define APP_NET_TEST_CLIENT_PAYLOAD_SIZE 64

static const char *TAG = "app_net_test";
static TaskHandle_t s_task;
static u32 s_tcp_echo_count;
static u32 s_udp_echo_count;
static u32 s_tcp_send_failures;
static u32 s_udp_send_failures;
static u32 s_client_pass_count;
static u32 s_client_mismatch_count;
static u32 s_client_sequence;
static u32 s_client_connection_id;
static u8 s_client_expected[APP_NET_TEST_CLIENT_PAYLOAD_SIZE];
static size_t s_client_expected_offset;
static TickType_t s_client_next_send;

static void app_net_test_fill_client_payload(u32 sequence, u8 *payload) {
    memcpy(payload, "BNET", 4);
    memcpy(payload + 4, &sequence, sizeof(sequence));
    for (size_t index = 8; index < APP_NET_TEST_CLIENT_PAYLOAD_SIZE; ++index) {
        payload[index] = (u8)(0x40u + ((sequence * 17u + (u32)index * 13u) & 0x3fu));
    }
}

static void app_net_test_tcp_echo(bsp_net_endpoint_id_t endpoint) {
    bsp_net_peer_t peers[BSP_NET_MAX_TCP_CONNECTIONS];
    size_t count = 0;
    if (bsp_net_tcp_peers(endpoint, peers, BSP_NET_MAX_TCP_CONNECTIONS, &count) != ESP_OK) {
        return;
    }
    for (size_t peer_index = 0; peer_index < count; ++peer_index) {
        if (!peers[peer_index].connected) {
            continue;
        }
        for (size_t round = 0; round < 4; ++round) {
            u8 data[APP_NET_TEST_TCP_READ_SIZE];
            size_t received = 0;
            const esp_err_t result = bsp_net_tcp_recv(&peers[peer_index], data, sizeof(data),
                                                      &received, 0);
            if (result != ESP_OK) {
                break;
            }
            if (bsp_net_send(&peers[peer_index], data, received, 100) == ESP_OK) {
                ++s_tcp_echo_count;
            } else {
                ++s_tcp_send_failures;
            }
        }
    }
}

static void app_net_test_udp_echo(void) {
    for (size_t round = 0; round < 4; ++round) {
        bsp_net_datagram_t datagram;
        const esp_err_t result = bsp_net_udp_recv(APP_NET_TEST_UDP_ENDPOINT, &datagram, 0);
        if (result != ESP_OK) {
            return;
        }
        if (bsp_net_send(&datagram.peer, datagram.data, datagram.len, 100) == ESP_OK) {
            ++s_udp_echo_count;
        } else {
            ++s_udp_send_failures;
        }
    }
}

static void app_net_test_client(void) {
#if CONFIG_BF_NET_TEST_CLIENT_ENABLED
    bsp_net_peer_t peers[BSP_NET_MAX_TCP_CONNECTIONS];
    size_t count = 0;
    if (bsp_net_tcp_peers(APP_NET_TEST_CLIENT_ENDPOINT, peers, BSP_NET_MAX_TCP_CONNECTIONS,
                          &count) != ESP_OK || count == 0) {
        s_client_connection_id = 0;
        s_client_expected_offset = 0;
        return;
    }
    bsp_net_peer_t *peer = &peers[0];
    if (peer->connection_id != s_client_connection_id) {
        s_client_connection_id = peer->connection_id;
        s_client_expected_offset = 0;
        s_client_next_send = xTaskGetTickCount();
    }
    if (!peer->connected) {
        return;
    }
    if (s_client_expected_offset == 0 &&
        (int32_t)(xTaskGetTickCount() - s_client_next_send) >= 0) {
        app_net_test_fill_client_payload(++s_client_sequence, s_client_expected);
        if (bsp_net_send(peer, s_client_expected, sizeof(s_client_expected), 100) == ESP_OK) {
            s_client_expected_offset = 1;
            s_client_next_send = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        }
    }
    if (s_client_expected_offset == 0) {
        return;
    }
    u8 received[APP_NET_TEST_CLIENT_PAYLOAD_SIZE];
    size_t length = 0;
    if (bsp_net_tcp_recv(peer, received, sizeof(received), &length, 0) != ESP_OK) {
        return;
    }
    const size_t expected_offset = s_client_expected_offset - 1;
    if (length > sizeof(s_client_expected) - expected_offset ||
        memcmp(received, s_client_expected + expected_offset, length) != 0) {
        ++s_client_mismatch_count;
        ESP_LOGE(TAG, "tcp client echo mismatch: sequence=%" PRIu32, s_client_sequence);
        s_client_expected_offset = 0;
        return;
    }
    s_client_expected_offset += length;
    if (s_client_expected_offset > sizeof(s_client_expected)) {
        ++s_client_pass_count;
        s_client_expected_offset = 0;
    }
#endif
}

static void app_net_test_task(void *arg) {
    (void)arg;
    u32 last_report_total = 0;
    for (;;) {
        app_net_test_tcp_echo(APP_NET_TEST_TCP_ENDPOINT);
        app_net_test_udp_echo();
        app_net_test_client();
        const u32 total = s_tcp_echo_count + s_udp_echo_count + s_client_pass_count;
        if (total - last_report_total >= 32) {
            ESP_LOGI(TAG, "passed: tcp=%" PRIu32 " udp=%" PRIu32 " client=%" PRIu32
                     " send_failures=%" PRIu32 "/%" PRIu32 " client_mismatch=%" PRIu32,
                     s_tcp_echo_count, s_udp_echo_count, s_client_pass_count,
                     s_tcp_send_failures, s_udp_send_failures, s_client_mismatch_count);
            last_report_total = total;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_net_test_start(void) {
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const bsp_net_config_t configs[] = {
        {.mode = BSP_NET_TCP_SERVER, .local_port = CONFIG_BF_NET_TEST_TCP_PORT},
        {.mode = BSP_NET_UDP, .local_port = CONFIG_BF_NET_TEST_UDP_PORT},
#if CONFIG_BF_NET_TEST_CLIENT_ENABLED
        {.mode = BSP_NET_TCP_CLIENT,
         .local_port = 0,
         .remote_host = CONFIG_BF_NET_TEST_REMOTE_HOST,
         .remote_port = CONFIG_BF_NET_TEST_REMOTE_PORT},
#endif
    };
    const esp_err_t result = bsp_net_start(configs, sizeof(configs) / sizeof(configs[0]));
    if (result != ESP_OK) {
        return result;
    }
    if (xTaskCreate(app_net_test_task, "app_net_test", APP_NET_TEST_TASK_STACK, NULL,
                    APP_NET_TEST_TASK_PRIORITY, &s_task) != pdPASS) {
        bsp_net_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started: tcp/%d udp/%d%s", CONFIG_BF_NET_TEST_TCP_PORT,
             CONFIG_BF_NET_TEST_UDP_PORT,
#if CONFIG_BF_NET_TEST_CLIENT_ENABLED
             " with tcp client"
#else
             ""
#endif
    );
    return ESP_OK;
}

#else

esp_err_t app_net_test_start(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
