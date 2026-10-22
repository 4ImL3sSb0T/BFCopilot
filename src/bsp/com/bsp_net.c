#include "bsp_net.h"

#include "sdkconfig.h"

#if CONFIG_BF_WIFI_ENABLED

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include <sys/select.h>

#include "lwip/netdb.h"
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
#define BSP_NET_CONNECT_TIMEOUT_MS 1000
#define BSP_NET_RECV_POLL_MS 20

static const char *TAG = "bsp_net";

static StreamBufferHandle_t s_rx;
static StaticSemaphore_t s_lock_storage;
static StaticSemaphore_t s_lifecycle_lock_storage;
static StaticSemaphore_t s_recv_lock_storage;
static atomic_flag s_init_lock = ATOMIC_FLAG_INIT;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_lifecycle_lock;
static SemaphoreHandle_t s_recv_lock;
static TaskHandle_t s_task;
static int s_listen_fd = -1;
static int s_tcp_clients[BSP_NET_MAX_TCP_CLIENTS] = {-1, -1, -1, -1};
static bool s_tcp_close_pending[BSP_NET_MAX_TCP_CLIENTS];
static int s_out_fd = -1;
static bool s_out_close_pending;
static int s_connect_fd = -1;
static TickType_t s_connect_started;
static struct sockaddr_in s_out_peer;
static bool s_out_enabled;
static TickType_t s_out_retry_at;
static int s_udp_fd = -1;
static struct sockaddr_in s_udp_peer;
static bool s_udp_peer_valid;
static bool s_udp_peer_fixed;
static atomic_bool s_running;
static u32 s_generation;

static void bsp_net_init_locks(void) {
    while (atomic_flag_test_and_set(&s_init_lock)) {
        vTaskDelay(1);
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        s_lifecycle_lock = xSemaphoreCreateMutexStatic(&s_lifecycle_lock_storage);
        s_recv_lock = xSemaphoreCreateMutexStatic(&s_recv_lock_storage);
    }
    atomic_flag_clear(&s_init_lock);
}

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

    const struct timeval send_timeout = {.tv_sec = 0, .tv_usec = BSP_NET_SEND_TIMEOUT_US};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) != 0) {
        close(fd);
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

    char ip[16];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "tcp client %s:%u connected", ip, (unsigned)ntohs(from.sin_port));
}

static void bsp_net_close_client(int fd) {
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] == fd) {
            s_tcp_clients[i] = -1;
            s_tcp_close_pending[i] = false;
            found = true;
        }
    }
    if (found) {
        close(fd);
    }
    xSemaphoreGive(s_lock);
}

static void bsp_net_read_client(int fd) {
    u8 buf[BSP_NET_CHUNK_SIZE];
    const int received = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
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

static void bsp_net_read_udp(int fd) {
    u8 buf[BSP_NET_MAX_UDP_RX_SIZE + 1];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    const int received = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                                  (struct sockaddr *)&from, &from_len);
    if (received <= 0) {
        return;
    }
    if (received > BSP_NET_MAX_UDP_RX_SIZE) {
        ESP_LOGW(TAG, "udp datagram exceeds %u bytes, dropped", (unsigned)BSP_NET_MAX_UDP_RX_SIZE);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_udp_peer_fixed) {
        s_udp_peer = from;
        s_udp_peer_valid = true;
    }
    xSemaphoreGive(s_lock);

    if (xStreamBufferSpacesAvailable(s_rx) < (size_t)received) {
        ESP_LOGW(TAG, "rx buffer full, dropped udp datagram (%d bytes)", received);
        return;
    }
    bsp_net_push(buf, (size_t)received);
}

static void bsp_net_schedule_retry(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(BSP_NET_RECONNECT_MS);
    xSemaphoreGive(s_lock);
}

static void bsp_net_finish_connect(void) {
    const int fd = s_connect_fd;
    s_connect_fd = -1;
    const int flags = fcntl(fd, F_GETFL, 0);
    const struct timeval send_timeout = {.tv_sec = 0, .tv_usec = BSP_NET_SEND_TIMEOUT_US};
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) != 0) {
        close(fd);
        bsp_net_schedule_retry();
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        s_out_fd = fd;
        s_out_close_pending = false;
        ESP_LOGI(TAG, "tcp client connected");
    } else {
        close(fd);
    }
    xSemaphoreGive(s_lock);
}

static void bsp_net_try_connect(void) {
    struct sockaddr_in peer;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool retry = s_out_enabled && s_out_fd < 0 && s_connect_fd < 0 &&
                       (int32_t)(xTaskGetTickCount() - s_out_retry_at) >= 0;
    peer = s_out_peer;
    xSemaphoreGive(s_lock);
    if (!retry) {
        return;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "tcp client socket() failed: errno %d", errno);
        bsp_net_schedule_retry();
        return;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        bsp_net_schedule_retry();
        return;
    }
    const int result = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
    if (result != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "tcp client connect failed: errno %d", errno);
        close(fd);
        bsp_net_schedule_retry();
        return;
    }
    s_connect_fd = fd;
    s_connect_started = xTaskGetTickCount();
    if (result == 0) {
        bsp_net_finish_connect();
    }
}

static void bsp_net_check_connect(bool ready) {
    if (s_connect_fd < 0) {
        return;
    }
    if (ready) {
        int sock_err = 0;
        socklen_t err_len = sizeof(sock_err);
        if (getsockopt(s_connect_fd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) == 0 &&
            sock_err == 0) {
            bsp_net_finish_connect();
            return;
        }
        ESP_LOGW(TAG, "tcp client connect error (%d)", sock_err);
    } else if (xTaskGetTickCount() - s_connect_started < pdMS_TO_TICKS(BSP_NET_CONNECT_TIMEOUT_MS)) {
        return;
    } else {
        ESP_LOGW(TAG, "tcp client connect timeout");
    }
    close(s_connect_fd);
    s_connect_fd = -1;
    bsp_net_schedule_retry();
}

static void bsp_net_read_out(int fd) {
    u8 buf[BSP_NET_CHUNK_SIZE];
    const int received = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (received > 0) {
        bsp_net_push(buf, (size_t)received);
        return;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    close(fd);
    s_out_fd = -1;
    s_out_close_pending = false;
    xSemaphoreGive(s_lock);
    bsp_net_schedule_retry();
    ESP_LOGI(TAG, "tcp client link down, retrying in %d ms", BSP_NET_RECONNECT_MS);
}

static void bsp_net_reap_clients(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int index = 0; index < BSP_NET_MAX_TCP_CLIENTS; ++index) {
        if (s_tcp_close_pending[index]) {
            close(s_tcp_clients[index]);
            s_tcp_clients[index] = -1;
            s_tcp_close_pending[index] = false;
        }
    }
    if (s_out_close_pending) {
        close(s_out_fd);
        s_out_fd = -1;
        s_out_close_pending = false;
        s_out_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(BSP_NET_RECONNECT_MS);
    }
    xSemaphoreGive(s_lock);
}

static void bsp_net_task(void *arg) {
    (void)arg;

    while (s_running) {
        bsp_net_reap_clients();
        bsp_net_check_connect(false);
        bsp_net_try_connect();

        fd_set rfds;
        fd_set wfds;
        fd_set efds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);

        int clients[BSP_NET_MAX_TCP_CLIENTS];
        int out_fd;
        int listen_fd;
        int udp_fd;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(clients, s_tcp_clients, sizeof(clients));
        out_fd = s_out_fd;
        listen_fd = s_listen_fd;
        udp_fd = s_udp_fd;
        xSemaphoreGive(s_lock);

        int max_fd = -1;
        if (listen_fd >= 0) {
            FD_SET(listen_fd, &rfds);
            max_fd = listen_fd;
        }
        for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
            if (clients[i] >= 0) {
                FD_SET(clients[i], &rfds);
                if (clients[i] > max_fd) {
                    max_fd = clients[i];
                }
            }
        }
        if (out_fd >= 0) {
            FD_SET(out_fd, &rfds);
            if (out_fd > max_fd) {
                max_fd = out_fd;
            }
        }
        if (udp_fd >= 0) {
            FD_SET(udp_fd, &rfds);
            if (udp_fd > max_fd) {
                max_fd = udp_fd;
            }
        }
        if (s_connect_fd >= 0) {
            FD_SET(s_connect_fd, &wfds);
            FD_SET(s_connect_fd, &efds);
            if (s_connect_fd > max_fd) {
                max_fd = s_connect_fd;
            }
        }

        if (max_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        struct timeval timeout = {.tv_sec = 0, .tv_usec = BSP_NET_SELECT_MS * 1000};
        const int ready = select(max_fd + 1, &rfds, &wfds, &efds, &timeout);
        if (!s_running) {
            break;
        }
        if (ready < 0) {
            ESP_LOGD(TAG, "select failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_connect_fd >= 0) {
            bsp_net_check_connect(FD_ISSET(s_connect_fd, &wfds) || FD_ISSET(s_connect_fd, &efds));
        }
        if (ready == 0) {
            continue;
        }

        for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
            if (clients[i] >= 0 && FD_ISSET(clients[i], &rfds)) {
                bsp_net_read_client(clients[i]);
            }
        }
        if (out_fd >= 0 && FD_ISSET(out_fd, &rfds)) {
            bsp_net_read_out(out_fd);
        }
        if (udp_fd >= 0 && FD_ISSET(udp_fd, &rfds)) {
            bsp_net_read_udp(udp_fd);
        }
        if (listen_fd >= 0 && FD_ISSET(listen_fd, &rfds)) {
            bsp_net_accept();
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

static esp_err_t bsp_net_ensure_resources(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rx == NULL) {
        s_rx = xStreamBufferCreate(BSP_NET_RX_BUFFER_SIZE, 1);
        if (s_rx == NULL) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_task == NULL) {
        s_running = true;
        if (xTaskCreate(bsp_net_task, "bsp_net", BSP_NET_TASK_STACK, NULL,
                        BSP_NET_TASK_PRIORITY, &s_task) != pdPASS) {
            s_running = false;
            s_task = NULL;
            xSemaphoreGive(s_lock);
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t bsp_net_resolve(const char *host, u16 port, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        return ESP_OK;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || result == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    out->sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
    freeaddrinfo(result);
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
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return ESP_FAIL;
    }

    *out_fd = fd;
    return ESP_OK;
}

static void bsp_net_stop_locked(void);

static void bsp_net_stop_if_idle(void) {
    if (s_listen_fd < 0 && s_udp_fd < 0 && !s_out_enabled) {
        bsp_net_stop_locked();
    }
}

static esp_err_t bsp_net_tcp_server_start_locked(u16 port) {
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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_listen_fd = fd;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "tcp server listening on port %u", (unsigned)port);
    return ESP_OK;
}

static esp_err_t bsp_net_tcp_client_start_locked(const char *host, u16 port) {
    if (host == NULL || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_out_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    struct sockaddr_in peer;
    const esp_err_t resolve_err = bsp_net_resolve(host, port, &peer);
    if (resolve_err != ESP_OK) {
        ESP_LOGE(TAG, "cannot resolve \"%s\"", host);
        return resolve_err;
    }

    const esp_err_t err = bsp_net_ensure_resources();
    if (err != ESP_OK) {
        bsp_net_stop_if_idle();
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out_peer = peer;
    s_out_retry_at = xTaskGetTickCount();
    s_out_enabled = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "tcp client target %s:%u", host, (unsigned)port);
    return ESP_OK;
}

static esp_err_t bsp_net_udp_start_locked(u16 port) {
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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_udp_fd = fd;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "udp server listening on port %u", (unsigned)port);
    return ESP_OK;
}

esp_err_t bsp_net_tcp_server_start(u16 port) {
    bsp_net_init_locks();
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    const esp_err_t result = bsp_net_tcp_server_start_locked(port);
    xSemaphoreGive(s_lifecycle_lock);
    return result;
}

esp_err_t bsp_net_tcp_client_start(const char *host, u16 port) {
    bsp_net_init_locks();
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    const esp_err_t result = bsp_net_tcp_client_start_locked(host, port);
    xSemaphoreGive(s_lifecycle_lock);
    return result;
}

esp_err_t bsp_net_udp_start(u16 port) {
    bsp_net_init_locks();
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    const esp_err_t result = bsp_net_udp_start_locked(port);
    xSemaphoreGive(s_lifecycle_lock);
    return result;
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

    bsp_net_init_locks();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_udp_peer = peer;
    s_udp_peer_valid = true;
    s_udp_peer_fixed = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t bsp_net_udp_send_to(const char *ip, u16 port, const void *data, size_t len) {
    if (ip == NULL || port == 0 || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    bsp_net_init_locks();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running || s_udp_fd < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const int sent = sendto(s_udp_fd, data, len, 0, (struct sockaddr *)&dest, sizeof(dest));
    xSemaphoreGive(s_lock);
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_net_send(const void *data, size_t len) {
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bsp_net_init_locks();
    bool delivered = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        const int fd = s_tcp_clients[i];
        if (fd < 0 || s_tcp_close_pending[i]) {
            continue;
        }
        if (send(fd, data, len, 0) == (int)len) {
            delivered = true;
        } else {
            ESP_LOGW(TAG, "tcp send failed: errno %d, scheduling close", errno);
            s_tcp_close_pending[i] = true;
        }
    }
    if (s_udp_fd >= 0 && s_udp_peer_valid) {
        if (sendto(s_udp_fd, data, len, 0,
                   (struct sockaddr *)&s_udp_peer, sizeof(s_udp_peer)) == (int)len) {
            delivered = true;
        }
    }
    if (s_out_fd >= 0 && !s_out_close_pending) {
        if (send(s_out_fd, data, len, 0) == (int)len) {
            delivered = true;
        } else {
            ESP_LOGW(TAG, "tcp client send failed: errno %d, reconnecting", errno);
            s_out_close_pending = true;
        }
    }
    xSemaphoreGive(s_lock);

    return delivered ? ESP_OK : ESP_ERR_INVALID_STATE;
}

int bsp_net_recv(void *buf, size_t len, u32 timeout_ms) {
    if (buf == NULL || len == 0) {
        return -1;
    }
    bsp_net_init_locks();
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const u32 generation = s_generation;
    const bool running = s_running && s_rx != NULL;
    xSemaphoreGive(s_lock);
    if (!running) {
        return -1;
    }
    if (xSemaphoreTake(s_recv_lock, timeout) != pdTRUE) {
        return s_running ? 0 : -1;
    }

    int result = -1;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool valid = s_running && s_rx != NULL && generation == s_generation;
        StreamBufferHandle_t rx = s_rx;
        xSemaphoreGive(s_lock);
        if (!valid) {
            break;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t remaining = elapsed < timeout ? timeout - elapsed : 0;
        const TickType_t poll_ticks = pdMS_TO_TICKS(BSP_NET_RECV_POLL_MS) > 0 ?
                                      pdMS_TO_TICKS(BSP_NET_RECV_POLL_MS) : 1;
        const TickType_t wait = remaining < poll_ticks ? remaining : poll_ticks;
        const size_t received = xStreamBufferReceive(rx, buf, len, wait);
        if (received > 0) {
            result = (int)received;
            break;
        }
        if (xTaskGetTickCount() - started >= timeout) {
            result = s_running ? 0 : -1;
            break;
        }
    }
    xSemaphoreGive(s_recv_lock);
    return result;
}

size_t bsp_net_tcp_client_count(void) {
    size_t count = 0;
    bsp_net_init_locks();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] >= 0 && !s_tcp_close_pending[i]) {
            ++count;
        }
    }
    if (s_out_fd >= 0 && !s_out_close_pending) {
        ++count;
    }
    xSemaphoreGive(s_lock);
    return count;
}

static esp_err_t bsp_net_format_peer(const struct sockaddr_in *peer, char *ip, size_t size, u16 *port) {
    char address[16];
    if (inet_ntop(AF_INET, &peer->sin_addr, address, sizeof(address)) == NULL) {
        return ESP_FAIL;
    }
    const size_t length = strlen(address) + 1;
    if (size < length) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(ip, address, length);
    if (port != NULL) {
        *port = (u16)ntohs(peer->sin_port);
    }
    return ESP_OK;
}

esp_err_t bsp_net_tcp_get_peer(size_t index, char *ip, size_t size, u16 *port) {
    if (ip == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ip[0] = '\0';
    bsp_net_init_locks();
    int fd = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] < 0 || s_tcp_close_pending[i]) {
            continue;
        }
        if (index == 0) {
            fd = s_tcp_clients[i];
            break;
        }
        --index;
    }
    if (fd < 0 && s_out_fd >= 0 && !s_out_close_pending && index == 0) {
        fd = s_out_fd;
    }
    if (fd < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int result = getpeername(fd, (struct sockaddr *)&peer, &peer_len);
    xSemaphoreGive(s_lock);
    if (result != 0) {
        return ESP_FAIL;
    }
    return bsp_net_format_peer(&peer, ip, size, port);
}

esp_err_t bsp_net_udp_get_peer(char *ip, size_t size, u16 *port) {
    if (ip == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ip[0] = '\0';
    bsp_net_init_locks();
    struct sockaddr_in peer;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool valid = s_udp_peer_valid;
    peer = s_udp_peer;
    xSemaphoreGive(s_lock);

    if (!valid) {
        return ESP_ERR_NOT_FOUND;
    }
    return bsp_net_format_peer(&peer, ip, size, port);
}

bool bsp_net_running(void) {
    bsp_net_init_locks();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool running = s_running && (s_listen_fd >= 0 || s_udp_fd >= 0 || s_out_enabled);
    xSemaphoreGive(s_lock);
    return running;
}

static void bsp_net_stop_locked(void) {
    s_running = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_generation;
    xSemaphoreGive(s_lock);

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool stopped = s_task == NULL;
        xSemaphoreGive(s_lock);
        if (stopped) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreTake(s_recv_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    for (int i = 0; i < BSP_NET_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp_clients[i] >= 0) {
            close(s_tcp_clients[i]);
            s_tcp_clients[i] = -1;
        }
        s_tcp_close_pending[i] = false;
    }
    if (s_udp_fd >= 0) {
        close(s_udp_fd);
        s_udp_fd = -1;
    }
    if (s_out_fd >= 0) {
        close(s_out_fd);
        s_out_fd = -1;
    }
    if (s_connect_fd >= 0) {
        close(s_connect_fd);
        s_connect_fd = -1;
    }
    s_out_close_pending = false;
    s_out_enabled = false;
    s_udp_peer_valid = false;
    s_udp_peer_fixed = false;
    if (s_rx != NULL) {
        vStreamBufferDelete(s_rx);
        s_rx = NULL;
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_recv_lock);
}

void bsp_net_stop(void) {
    bsp_net_init_locks();
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    bsp_net_stop_locked();
    xSemaphoreGive(s_lifecycle_lock);
}

#else // !CONFIG_BF_WIFI_ENABLED

esp_err_t bsp_net_tcp_server_start(u16 port) {
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_tcp_client_start(const char *host, u16 port) {
    (void)host;
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

esp_err_t bsp_net_udp_send_to(const char *ip, u16 port, const void *data, size_t len) {
    (void)ip;
    (void)port;
    (void)data;
    (void)len;
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

esp_err_t bsp_net_tcp_get_peer(size_t index, char *ip, size_t size, u16 *port) {
    (void)index;
    (void)ip;
    (void)size;
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_udp_get_peer(char *ip, size_t size, u16 *port) {
    (void)ip;
    (void)size;
    (void)port;
    return ESP_ERR_NOT_SUPPORTED;
}

bool bsp_net_running(void) {
    return false;
}

void bsp_net_stop(void) {
}

#endif
