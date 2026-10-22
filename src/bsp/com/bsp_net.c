#include "bsp_net.h"

#include "sdkconfig.h"

#if CONFIG_BF_WIFI_ENABLED

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <sys/select.h>

#include "lwip/sockets.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "bsp_wifi.h"

#define BSP_NET_TASK_STACK    4096
#define BSP_NET_TASK_PRIORITY 6
#define BSP_NET_SELECT_MS     200
#define BSP_NET_CHUNK_SIZE    1024
#define BSP_NET_SEND_TIMEOUT_US (200 * 1000)

static const char *TAG = "bsp_net";

static StreamBufferHandle_t s_rx;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static int s_listen_fd = -1;
static int s_tcp_clients[BSP_NET_MAX_TCP_CLIENTS] = {-1, -1, -1, -1};
static int s_udp_fd = -1;
static struct sockaddr_in s_udp_peer;
static bool s_udp_peer_valid;
static volatile bool s_running;

static void bsp_net_push(const void *data, size_t len) {
    const size_t written = xStreamBufferSend(s_rx, data, len, 0);
    if (written < len) {
        ESP_LOGD(TAG, "rx buffer full, dropped %u bytes", (unsigned)(len - written));
    }
}

static void bsp_net_accept(void) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    const int fd = accept(s_listen_fd, (struct sockaddr *)&from, &from_len);
    if (fd < 0) {
        return;
    }

    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] < 0) {
            s_tcp_clients[i] = fd;
            slot = i;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (slot < 0) {
        close(fd);
        ESP_LOGW(TAG, "tcp client rejected: already %d connections", BSP_NET_MAX_TCP_CLIENTS);
        return;
    }

    // 对端不收数据时不要卡死接收任务；超时后 send() 失败，连接会被丢掉。
    const struct timeval send_timeout = {.tv_sec = 0, .tv_usec = BSP_NET_SEND_TIMEOUT_US};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    char ip[16];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "tcp client %s:%u connected", ip, (unsigned)ntohs(from.sin_port));
}

static void bsp_net_close_client(int fd) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] == fd) {
            s_tcp_clients[i] = -1;
        }
    }
    xSemaphoreGive(s_lock);
    close(fd);
}

static void bsp_net_read_client(int fd) {
    u8 buf[BSP_NET_CHUNK_SIZE];
    const int received = recv(fd, buf, sizeof(buf), 0);
    if (received > 0) {
        bsp_net_push(buf, (size_t)received);
        return;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    ESP_LOGI(TAG, "tcp client disconnected");
    bsp_net_close_client(fd);
}

static void bsp_net_read_udp(void) {
    u8 buf[BSP_NET_CHUNK_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    const int received = recvfrom(s_udp_fd, buf, sizeof(buf), 0,
                                  (struct sockaddr *)&from, &from_len);
    if (received <= 0) {
        return;
    }

    // 记住来源，回复默认发回这里；UDP 无连接，发不出去也只会丢包。
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_udp_peer = from;
    s_udp_peer_valid = true;
    xSemaphoreGive(s_lock);

    bsp_net_push(buf, (size_t)received);
}

static void bsp_net_task(void *arg) {
    (void)arg;

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);

        int clients[BSP_NET_MAX_TCP_CLIENTS];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(clients, s_tcp_clients, sizeof(clients));
        xSemaphoreGive(s_lock);

        int max_fd = -1;
        if (s_listen_fd >= 0) {
            FD_SET(s_listen_fd, &rfds);
            max_fd = s_listen_fd;
        }
        for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
            if (clients[i] >= 0) {
                FD_SET(clients[i], &rfds);
                if (clients[i] > max_fd) {
                    max_fd = clients[i];
                }
            }
        }
        if (s_udp_fd >= 0) {
            FD_SET(s_udp_fd, &rfds);
            if (s_udp_fd > max_fd) {
                max_fd = s_udp_fd;
            }
        }

        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        struct timeval timeout = {.tv_sec = 0, .tv_usec = BSP_NET_SELECT_MS * 1000};
        const int ready = select(max_fd + 1, &rfds, NULL, NULL, &timeout);
        if (ready < 0) {
            // bsp_net_send() 可能刚关掉一个客户端 fd，select 会报 EBADF；重建 fd 集合即可。
            if (errno != EBADF) {
                ESP_LOGD(TAG, "select failed: errno %d", errno);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ready == 0) {
            continue;
        }

        if (s_listen_fd >= 0 && FD_ISSET(s_listen_fd, &rfds)) {
            bsp_net_accept();
        }
        for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
            if (clients[i] >= 0 && FD_ISSET(clients[i], &rfds)) {
                bsp_net_read_client(clients[i]);
            }
        }
        if (s_udp_fd >= 0 && FD_ISSET(s_udp_fd, &rfds)) {
            bsp_net_read_udp();
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t bsp_net_ensure_resources(void) {
    if (s_rx == NULL) {
        s_rx = xStreamBufferCreate(BSP_NET_RX_BUFFER_SIZE, 1);
        if (s_rx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_task == NULL) {
        s_running = true;
        if (xTaskCreate(bsp_net_task, "bsp_net", BSP_NET_TASK_STACK, NULL,
                        BSP_NET_TASK_PRIORITY, &s_task) != pdPASS) {
            s_running = false;
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t bsp_net_open_socket(u16 port, int type, int *out_fd) {
    const int protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    const int fd = socket(AF_INET, type, protocol);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    const int enable = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (type == SOCK_DGRAM) {
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind port %u failed: errno %d", (unsigned)port, errno);
        close(fd);
        return ESP_FAIL;
    }

    *out_fd = fd;
    return ESP_OK;
}

static void bsp_net_stop_if_idle(void) {
    if (s_listen_fd < 0 && s_udp_fd < 0) {
        bsp_net_stop();
    }
}

esp_err_t bsp_net_tcp_server_start(u16 port) {
    if (port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_listen_fd >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = bsp_net_ensure_resources();
    if (err != ESP_OK) {
        bsp_net_stop_if_idle();
        return err;
    }

    int fd = -1;
    err = bsp_net_open_socket(port, SOCK_STREAM, &fd);
    if (err != ESP_OK) {
        bsp_net_stop_if_idle();
        return err;
    }
    if (listen(fd, 1) != 0) {
        ESP_LOGE(TAG, "listen port %u failed: errno %d", (unsigned)port, errno);
        close(fd);
        bsp_net_stop_if_idle();
        return ESP_FAIL;
    }

    s_listen_fd = fd;
    ESP_LOGI(TAG, "tcp server listening on port %u", (unsigned)port);
    return ESP_OK;
}

esp_err_t bsp_net_udp_start(u16 port) {
    if (port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_udp_fd >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = bsp_net_ensure_resources();
    if (err != ESP_OK) {
        bsp_net_stop_if_idle();
        return err;
    }

    int fd = -1;
    err = bsp_net_open_socket(port, SOCK_DGRAM, &fd);
    if (err != ESP_OK) {
        bsp_net_stop_if_idle();
        return err;
    }

    s_udp_fd = fd;
    ESP_LOGI(TAG, "udp server listening on port %u", (unsigned)port);
    return ESP_OK;
}

esp_err_t bsp_net_udp_set_peer(const char *ip, u16 port) {
    if (ip == NULL || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &peer.sin_addr) != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    // 允许在 udp_start() 之前先设对端，所以锁在这里按需创建。
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_udp_peer = peer;
    s_udp_peer_valid = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t bsp_net_send(const void *data, size_t len) {
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    bool delivered = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        const int fd = s_tcp_clients[i];
        if (fd < 0) {
            continue;
        }
        if (send(fd, data, len, 0) == (int)len) {
            delivered = true;
        } else {
            // 半包说明链路已经不可用，直接丢掉这个连接，避免后续继续写。
            ESP_LOGW(TAG, "tcp send failed: errno %d, closing client", errno);
            s_tcp_clients[i] = -1;
            close(fd);
        }
    }
    if (s_udp_fd >= 0 && s_udp_peer_valid) {
        if (sendto(s_udp_fd, data, len, 0,
                   (struct sockaddr *)&s_udp_peer, sizeof(s_udp_peer)) == (int)len) {
            delivered = true;
        }
    }
    xSemaphoreGive(s_lock);

    return delivered ? ESP_OK : ESP_ERR_INVALID_STATE;
}

int bsp_net_recv(void *buf, size_t len, u32 timeout_ms) {
    if (s_rx == NULL || buf == NULL || len == 0) {
        return -1;
    }
    const size_t received = xStreamBufferReceive(s_rx, buf, len, pdMS_TO_TICKS(timeout_ms));
    return (int)received;
}

size_t bsp_net_tcp_client_count(void) {
    size_t count = 0;
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] >= 0) {
            ++count;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

bool bsp_net_running(void) {
    return s_running && (s_listen_fd >= 0 || s_udp_fd >= 0);
}

void bsp_net_stop(void) {
    s_running = false;

    bool stopped = s_task == NULL;
    for (int i = 0; !stopped && i < 50; ++i) {
        if (s_task == NULL) {
            stopped = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] >= 0) {
            close(s_tcp_clients[i]);
            s_tcp_clients[i] = -1;
        }
    }
    if (s_udp_fd >= 0) {
        close(s_udp_fd);
        s_udp_fd = -1;
    }
    s_udp_peer_valid = false;

    if (!stopped) {
        // 任务还没退出就释放缓冲会踩到它，宁可留着内存也不冒险。
        ESP_LOGW(TAG, "task still running after stop; buffers kept");
        return;
    }
    if (s_rx != NULL) {
        vStreamBufferDelete(s_rx);
        s_rx = NULL;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
}

#else // !CONFIG_BF_WIFI_ENABLED

esp_err_t bsp_net_tcp_server_start(u16 port) {
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_udp_start(u16 port) {
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_udp_set_peer(const char *ip, u16 port) {
    (void)ip;
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_send(const void *data, size_t len) {
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

int bsp_net_recv(void *buf, size_t len, u32 timeout_ms) {
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return -1;
}

size_t bsp_net_tcp_client_count(void) {
    return 0;
}

bool bsp_net_running(void) {
    return false;
}

void bsp_net_stop(void) {
}

#endif
