#include "bsp_net.h"

#include "sdkconfig.h"

#if CONFIG_BF_WIFI_ENABLED

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "bsp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#define BSP_NET_TASK_STACK 4096
#define BSP_NET_TASK_PRIORITY 6
#define BSP_NET_POLL_MS 10
#define BSP_NET_CHANNEL_COUNT (BSP_NET_MAX_TCP_CONNECTIONS + BSP_NET_MAX_ENDPOINTS)

typedef struct {
    struct sockaddr_in address;
    size_t len;
    u8 data[BSP_NET_MAX_PAYLOAD];
} bsp_net_tx_request_t;

typedef struct {
    struct sockaddr_in address;
    size_t len;
} bsp_net_udp_header_t;

typedef struct {
    int fd;
    StreamBufferHandle_t rx;
    QueueHandle_t tx;
    StaticSemaphore_t recv_storage;
    SemaphoreHandle_t recv_lock;
    bsp_net_peer_t peer;
    size_t tx_offset;
    TickType_t tx_started;
    bool tx_active;
    bool reserved;
    bsp_net_udp_header_t udp_header;
    bool udp_pending;
} bsp_net_channel_t;

typedef struct {
    bsp_net_mode_t mode;
    u16 local_port;
    int fd;
    struct sockaddr_in remote;
    TickType_t connect_started;
    TickType_t retry_at;
    bsp_net_channel_t *client;
} bsp_net_endpoint_t;

static const char *TAG = "bsp_net";
static bsp_net_channel_t s_channels[BSP_NET_CHANNEL_COUNT];
static bsp_net_endpoint_t s_endpoints[BSP_NET_MAX_ENDPOINTS];
static size_t s_endpoint_count;
static StaticSemaphore_t s_lock_storage;
static StaticSemaphore_t s_lifecycle_storage;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_lifecycle_lock;
static atomic_flag s_init_lock = ATOMIC_FLAG_INIT;
static atomic_bool s_running;
static TaskHandle_t s_task;
static u32 s_epoch;
static u32 s_next_connection_id;

static TickType_t bsp_net_ticks(u32 timeout_ms) {
    const uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999) / 1000;
    return ticks < portMAX_DELAY ? (TickType_t)ticks : portMAX_DELAY - 1;
}

static void bsp_net_init_locks(void) {
    while (atomic_flag_test_and_set(&s_init_lock)) {
        vTaskDelay(1);
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        s_lifecycle_lock = xSemaphoreCreateMutexStatic(&s_lifecycle_storage);
        for (size_t index = 0; index < BSP_NET_CHANNEL_COUNT; ++index) {
            s_channels[index].fd = -1;
            s_channels[index].recv_lock = xSemaphoreCreateMutexStatic(&s_channels[index].recv_storage);
        }
        for (size_t index = 0; index < BSP_NET_MAX_ENDPOINTS; ++index) {
            s_endpoints[index].fd = -1;
        }
    }
    atomic_flag_clear(&s_init_lock);
}

static bool bsp_net_temporary_error(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ENOBUFS;
}

static bsp_net_channel_t *bsp_net_find_tcp(const bsp_net_peer_t *peer) {
    if (peer->connection_id != 0) {
        for (size_t index = 0; index < BSP_NET_MAX_TCP_CONNECTIONS; ++index) {
            if (s_channels[index].peer.connection_id == peer->connection_id &&
                s_channels[index].peer.endpoint == peer->endpoint) {
                return &s_channels[index];
            }
        }
    }
    return NULL;
}

static bool bsp_net_address(const char *ip, u16 port, struct sockaddr_in *address) {
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    return ip != NULL && port != 0 && inet_pton(AF_INET, ip, &address->sin_addr) == 1;
}

static esp_err_t bsp_net_resolve(const char *host, u16 port, struct sockaddr_in *address) {
    if (bsp_net_address(host, port, address)) {
        return ESP_OK;
    }
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || result == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    address->sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return ESP_OK;
}

static bool bsp_net_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int bsp_net_open(u16 port, bool udp) {
    const int fd = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    const int enable = 1;
    const struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0 ||
        (udp && setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) != 0) ||
        !bsp_net_nonblocking(fd) || bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        (!udp && listen(fd, BSP_NET_MAX_TCP_CONNECTIONS) != 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void bsp_net_close_tcp(bsp_net_channel_t *channel) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int fd = channel->fd;
    channel->fd = -1;
    channel->peer.connected = false;
    channel->tx_active = false;
    channel->tx_offset = 0;
    xQueueReset(channel->tx);
    xSemaphoreGive(s_lock);
    if (fd >= 0) {
        close(fd);
    }
    bsp_net_endpoint_t *endpoint = &s_endpoints[channel->peer.endpoint];
    if (endpoint->mode == BSP_NET_TCP_CLIENT) {
        endpoint->retry_at = xTaskGetTickCount() + bsp_net_ticks(BSP_NET_RECONNECT_MS);
    }
}

static bool bsp_net_attach_tcp(bsp_net_channel_t *channel, bsp_net_endpoint_id_t endpoint,
                               int fd, const struct sockaddr_in *address) {
    if (xSemaphoreTake(channel->recv_lock, 0) != pdTRUE) {
        return false;
    }
    bool attached = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running && channel->fd < 0 && xStreamBufferBytesAvailable(channel->rx) == 0) {
        const int enable = 1;
        if (bsp_net_nonblocking(fd) &&
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) == 0 &&
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable)) == 0) {
            xStreamBufferReset(channel->rx);
            if (++s_next_connection_id == 0) {
                ++s_next_connection_id;
            }
            channel->peer = (bsp_net_peer_t){
                .endpoint = endpoint, .connection_id = s_next_connection_id,
                .port = ntohs(address->sin_port), .connected = true,
            };
            inet_ntop(AF_INET, &address->sin_addr, channel->peer.ip, sizeof(channel->peer.ip));
            channel->fd = fd;
            attached = true;
        }
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(channel->recv_lock);
    return attached;
}

static void bsp_net_accept(bsp_net_endpoint_id_t endpoint) {
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    const int fd = accept(s_endpoints[endpoint].fd, (struct sockaddr *)&address, &size);
    if (fd < 0) {
        return;
    }
    for (size_t index = 0; index < BSP_NET_MAX_TCP_CONNECTIONS; ++index) {
        if (!s_channels[index].reserved && bsp_net_attach_tcp(&s_channels[index], endpoint, fd, &address)) {
            return;
        }
    }
    close(fd);
    ESP_LOGD(TAG, "tcp connection rejected: no free receive slot");
}

static void bsp_net_connect(bsp_net_endpoint_id_t id, bool ready) {
    bsp_net_endpoint_t *endpoint = &s_endpoints[id];
    bsp_net_channel_t *channel = endpoint->client;
    const TickType_t now = xTaskGetTickCount();
    if (endpoint->fd < 0) {
        if (channel->fd >= 0 || xStreamBufferBytesAvailable(channel->rx) != 0 ||
            (int32_t)(now - endpoint->retry_at) < 0) {
            return;
        }
        endpoint->retry_at = now + bsp_net_ticks(BSP_NET_RECONNECT_MS);
        endpoint->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (endpoint->fd < 0) {
            return;
        }
        const struct sockaddr_in local = {
            .sin_family = AF_INET, .sin_port = htons(endpoint->local_port), .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        const int enable = 1;
        if (bsp_net_nonblocking(endpoint->fd) &&
            setsockopt(endpoint->fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == 0 &&
            bind(endpoint->fd, (const struct sockaddr *)&local, sizeof(local)) == 0) {
            const int result = connect(endpoint->fd, (struct sockaddr *)&endpoint->remote, sizeof(endpoint->remote));
            if (result == 0 || errno == EINPROGRESS) {
                endpoint->connect_started = now;
                if (result != 0) {
                    return;
                }
                ready = true;
            }
        }
        if (!ready) {
            close(endpoint->fd);
            endpoint->fd = -1;
            return;
        }
    }
    if (ready) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(endpoint->fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0 &&
            bsp_net_attach_tcp(channel, id, endpoint->fd, &endpoint->remote)) {
            endpoint->fd = -1;
            return;
        }
    } else if (now - endpoint->connect_started < bsp_net_ticks(BSP_NET_CONNECT_TIMEOUT_MS)) {
        return;
    }
    close(endpoint->fd);
    endpoint->fd = -1;
    endpoint->retry_at = now + bsp_net_ticks(BSP_NET_RECONNECT_MS);
}

static void bsp_net_read_tcp(bsp_net_channel_t *channel) {
    u8 data[BSP_NET_MAX_PAYLOAD];
    const size_t available = xStreamBufferSpacesAvailable(channel->rx);
    if (available == 0) {
        return;
    }
    const int received = recv(channel->fd, data, available < sizeof(data) ? available : sizeof(data), MSG_DONTWAIT);
    if (received > 0) {
        xStreamBufferSend(channel->rx, data, (size_t)received, 0);
    } else if (received == 0 || !bsp_net_temporary_error(errno)) {
        bsp_net_close_tcp(channel);
    }
}

static void bsp_net_read_udp(bsp_net_channel_t *channel) {
    u8 data[BSP_NET_MAX_PAYLOAD];
    bsp_net_udp_header_t header = {0};
    struct iovec buffer = {.iov_base = data, .iov_len = sizeof(data)};
    struct msghdr message = {
        .msg_name = &header.address, .msg_namelen = sizeof(header.address), .msg_iov = &buffer, .msg_iovlen = 1,
    };
    const int received = recvmsg(channel->fd, &message, MSG_DONTWAIT);
    if (received < 0 || (message.msg_flags & MSG_TRUNC) != 0 || received > BSP_NET_MAX_PAYLOAD) {
        return;
    }
    header.len = (size_t)received;
    const size_t required = sizeof(header) + header.len +
                            sizeof(configMESSAGE_BUFFER_LENGTH_TYPE) * (header.len != 0 ? 2 : 1);
    if (xMessageBufferSpacesAvailable(channel->rx) < required) {
        return;
    }
    xMessageBufferSend(channel->rx, &header, sizeof(header), 0);
    if (header.len != 0) {
        xMessageBufferSend(channel->rx, data, header.len, 0);
    }
}

static void bsp_net_write(bsp_net_channel_t *channel, bool udp) {
    bsp_net_tx_request_t request;
    if (xQueuePeek(channel->tx, &request, 0) != pdTRUE) {
        return;
    }
    const int sent = udp ?
        sendto(channel->fd, request.data, request.len, MSG_DONTWAIT,
               (struct sockaddr *)&request.address, sizeof(request.address)) :
        send(channel->fd, request.data + channel->tx_offset, request.len - channel->tx_offset, MSG_DONTWAIT);
    if (!udp && sent > 0) {
        channel->tx_offset += (size_t)sent;
    }
    if (udp || channel->tx_offset == request.len) {
        xQueueReceive(channel->tx, &request, 0);
        channel->tx_offset = 0;
        channel->tx_active = false;
    } else if (sent == 0 || (sent < 0 && !bsp_net_temporary_error(errno))) {
        bsp_net_close_tcp(channel);
    }
}

static void bsp_net_watch_tx(bsp_net_channel_t *channel) {
    if (!channel->tx_active && uxQueueMessagesWaiting(channel->tx) != 0) {
        channel->tx_active = true;
        channel->tx_started = xTaskGetTickCount();
    }
    if (channel->tx_active && xTaskGetTickCount() - channel->tx_started >=
        bsp_net_ticks(BSP_NET_TCP_SEND_TIMEOUT_MS)) {
        bsp_net_close_tcp(channel);
    }
}

static void bsp_net_add_fd(int fd, fd_set *set, int *max_fd) {
    if (fd >= 0) {
        FD_SET(fd, set);
        if (fd > *max_fd) {
            *max_fd = fd;
        }
    }
}

static void bsp_net_task(void *arg) {
    (void)arg;
    unsigned busy_rounds = 0;
    while (s_running) {
        fd_set readers;
        fd_set writers;
        fd_set errors;
        FD_ZERO(&readers);
        FD_ZERO(&writers);
        FD_ZERO(&errors);
        int max_fd = -1;
        for (size_t index = 0; index < s_endpoint_count; ++index) {
            bsp_net_endpoint_t *endpoint = &s_endpoints[index];
            if (endpoint->mode == BSP_NET_TCP_SERVER) {
                bsp_net_add_fd(endpoint->fd, &readers, &max_fd);
            } else if (endpoint->mode == BSP_NET_TCP_CLIENT) {
                bsp_net_connect((bsp_net_endpoint_id_t)index, false);
                bsp_net_add_fd(endpoint->fd, &writers, &max_fd);
                bsp_net_add_fd(endpoint->fd, &errors, &max_fd);
            }
        }
        for (size_t index = 0; index < BSP_NET_CHANNEL_COUNT; ++index) {
            bsp_net_channel_t *channel = &s_channels[index];
            if (channel->fd < 0) {
                continue;
            }
            const bool udp = index >= BSP_NET_MAX_TCP_CONNECTIONS;
            if (!udp) {
                bsp_net_watch_tx(channel);
                if (channel->fd < 0) {
                    continue;
                }
            }
            if (udp || xStreamBufferSpacesAvailable(channel->rx) > 0) {
                bsp_net_add_fd(channel->fd, &readers, &max_fd);
            }
            if (uxQueueMessagesWaiting(channel->tx) != 0) {
                bsp_net_add_fd(channel->fd, &writers, &max_fd);
            }
        }
        struct timeval timeout = {0};
        const int ready = max_fd < 0 ? 0 : select(max_fd + 1, &readers, &writers, &errors, &timeout);
        if (!s_running) {
            break;
        }
        if (ready > 0) {
            for (size_t index = 0; index < BSP_NET_CHANNEL_COUNT; ++index) {
                bsp_net_channel_t *channel = &s_channels[index];
                const bool udp = index >= BSP_NET_MAX_TCP_CONNECTIONS;
                if (channel->fd >= 0 && FD_ISSET(channel->fd, &readers)) {
                    if (udp) {
                        bsp_net_read_udp(channel);
                    } else {
                        bsp_net_read_tcp(channel);
                    }
                }
                if (channel->fd >= 0 && FD_ISSET(channel->fd, &writers)) {
                    bsp_net_write(channel, udp);
                }
            }
            for (size_t index = 0; index < s_endpoint_count; ++index) {
                bsp_net_endpoint_t *endpoint = &s_endpoints[index];
                if (endpoint->fd < 0) {
                    continue;
                }
                if (endpoint->mode == BSP_NET_TCP_SERVER && FD_ISSET(endpoint->fd, &readers)) {
                    bsp_net_accept((bsp_net_endpoint_id_t)index);
                } else if (endpoint->mode == BSP_NET_TCP_CLIENT &&
                           (FD_ISSET(endpoint->fd, &writers) || FD_ISSET(endpoint->fd, &errors))) {
                    bsp_net_connect((bsp_net_endpoint_id_t)index, true);
                }
            }
            if (++busy_rounds == 16) {
                vTaskDelay(1);
                busy_rounds = 0;
            }
        } else {
            busy_rounds = 0;
            ulTaskNotifyTake(pdTRUE, bsp_net_ticks(BSP_NET_POLL_MS));
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

static void bsp_net_release(void) {
    for (size_t index = 0; index < s_endpoint_count; ++index) {
        if (s_endpoints[index].fd >= 0) {
            close(s_endpoints[index].fd);
        }
        s_endpoints[index] = (bsp_net_endpoint_t){.fd = -1};
    }
    for (size_t index = 0; index < BSP_NET_CHANNEL_COUNT; ++index) {
        bsp_net_channel_t *channel = &s_channels[index];
        if (channel->fd >= 0) {
            close(channel->fd);
        }
        if (channel->rx != NULL) {
            vStreamBufferDelete(channel->rx);
        }
        if (channel->tx != NULL) {
            vQueueDelete(channel->tx);
        }
        channel->fd = -1;
        channel->rx = NULL;
        channel->tx = NULL;
        channel->peer = (bsp_net_peer_t){0};
        channel->tx_offset = 0;
        channel->tx_active = false;
        channel->reserved = false;
        channel->udp_pending = false;
    }
    s_endpoint_count = 0;
}

esp_err_t bsp_net_start(const bsp_net_config_t *configs, size_t count) {
    if (configs == NULL || count == 0 || count > BSP_NET_MAX_ENDPOINTS) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t clients = 0;
    bool server = false;
    for (size_t index = 0; index < count; ++index) {
        const bsp_net_config_t *config = &configs[index];
        if (config->mode < BSP_NET_TCP_SERVER || config->mode > BSP_NET_UDP ||
            (config->mode != BSP_NET_TCP_CLIENT && config->local_port == 0) ||
            (config->mode == BSP_NET_TCP_CLIENT &&
             (config->remote_host == NULL || config->remote_host[0] == '\0' || config->remote_port == 0))) {
            return ESP_ERR_INVALID_ARG;
        }
        clients += config->mode == BSP_NET_TCP_CLIENT;
        server |= config->mode == BSP_NET_TCP_SERVER;
        for (size_t previous = 0; previous < index; ++previous) {
            if (config->local_port != 0 && config->local_port == configs[previous].local_port &&
                (config->mode == BSP_NET_UDP) == (configs[previous].mode == BSP_NET_UDP)) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    if (clients + (server ? 1 : 0) > BSP_NET_MAX_TCP_CONNECTIONS) {
        return ESP_ERR_INVALID_SIZE;
    }
    bsp_net_init_locks();
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_running || !bsp_wifi_is_connected()) {
        goto done;
    }
    s_endpoint_count = count;
    const size_t tcp_count = server ? BSP_NET_MAX_TCP_CONNECTIONS : clients;
    result = ESP_ERR_NO_MEM;
    for (size_t index = 0; index < BSP_NET_CHANNEL_COUNT; ++index) {
        const bool udp = index >= BSP_NET_MAX_TCP_CONNECTIONS;
        const size_t endpoint = udp ? index - BSP_NET_MAX_TCP_CONNECTIONS : 0;
        if (index >= tcp_count && !(udp && endpoint < count && configs[endpoint].mode == BSP_NET_UDP)) {
            continue;
        }
        bsp_net_channel_t *channel = &s_channels[index];
        channel->rx = udp ? xMessageBufferCreate(BSP_NET_RX_BUFFER_SIZE) : xStreamBufferCreate(BSP_NET_RX_BUFFER_SIZE, 1);
        channel->tx = xQueueCreate(BSP_NET_TX_QUEUE_DEPTH, sizeof(bsp_net_tx_request_t));
        if (channel->rx == NULL || channel->tx == NULL) {
            goto failed;
        }
    }
    result = ESP_FAIL;
    size_t client_index = 0;
    for (size_t index = 0; index < count; ++index) {
        const bsp_net_config_t *config = &configs[index];
        bsp_net_endpoint_t *endpoint = &s_endpoints[index];
        endpoint->mode = config->mode;
        endpoint->local_port = config->local_port;
        if (config->mode == BSP_NET_TCP_CLIENT) {
            result = bsp_net_resolve(config->remote_host, config->remote_port, &endpoint->remote);
            if (result != ESP_OK) {
                goto failed;
            }
            endpoint->client = &s_channels[client_index++];
            endpoint->client->reserved = true;
            endpoint->retry_at = xTaskGetTickCount();
        } else {
            const bool udp = config->mode == BSP_NET_UDP;
            const int fd = bsp_net_open(config->local_port, udp);
            result = ESP_FAIL;
            if (fd < 0) {
                goto failed;
            }
            if (udp) {
                bsp_net_channel_t *channel = &s_channels[BSP_NET_MAX_TCP_CONNECTIONS + index];
                channel->fd = fd;
                channel->peer.endpoint = (bsp_net_endpoint_id_t)index;
            } else {
                endpoint->fd = fd;
            }
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_running = true;
    if (xTaskCreate(bsp_net_task, "bsp_net", BSP_NET_TASK_STACK, NULL, BSP_NET_TASK_PRIORITY, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        xSemaphoreGive(s_lock);
        result = ESP_ERR_NO_MEM;
        goto failed;
    }
    xSemaphoreGive(s_lock);
    result = ESP_OK;
    goto done;
failed:
    bsp_net_release();
done:
    xSemaphoreGive(s_lifecycle_lock);
    return result;
}

void bsp_net_stop(void) {
    bsp_net_init_locks();
    xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_running = false;
    ++s_epoch;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool stopped = s_task == NULL;
        xSemaphoreGive(s_lock);
        if (stopped) {
            break;
        }
        vTaskDelay(1);
    }
    for (size_t index = 0; index < BSP_NET_CHANNEL_COUNT; ++index) {
        xSemaphoreTake(s_channels[index].recv_lock, portMAX_DELAY);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bsp_net_release();
    xSemaphoreGive(s_lock);
    for (size_t index = 0; index < BSP_NET_CHANNEL_COUNT; ++index) {
        xSemaphoreGive(s_channels[index].recv_lock);
    }
    xSemaphoreGive(s_lifecycle_lock);
}

bool bsp_net_running(void) {
    return s_running;
}

esp_err_t bsp_net_send(const bsp_net_peer_t *peer, const void *data, size_t len, u32 timeout_ms) {
    if (peer == NULL || peer->endpoint >= BSP_NET_MAX_ENDPOINTS || (data == NULL && len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > BSP_NET_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    bsp_net_tx_request_t request = {.len = len};
    const bsp_net_peer_t target = *peer;
    if (target.connection_id == 0 &&
        (memchr(target.ip, '\0', sizeof(target.ip)) == NULL || !bsp_net_address(target.ip, target.port, &request.address))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != 0) {
        memcpy(request.data, data, len);
    }
    bsp_net_init_locks();
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = bsp_net_ticks(timeout_ms);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const u32 epoch = s_epoch;
    xSemaphoreGive(s_lock);
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bsp_net_channel_t *channel = NULL;
        if (s_running && epoch == s_epoch && target.endpoint < s_endpoint_count) {
            const bool udp = s_endpoints[target.endpoint].mode == BSP_NET_UDP;
            if ((udp && target.connection_id != 0) || (!udp && (target.connection_id == 0 || len == 0))) {
                xSemaphoreGive(s_lock);
                return ESP_ERR_INVALID_ARG;
            }
            channel = udp ? &s_channels[BSP_NET_MAX_TCP_CONNECTIONS + target.endpoint] : bsp_net_find_tcp(&target);
        }
        if (channel == NULL || channel->fd < 0) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        const bool queued = xQueueSend(channel->tx, &request, 0) == pdTRUE;
        if (queued) {
            xTaskNotifyGive(s_task);
        }
        xSemaphoreGive(s_lock);
        if (queued) {
            return ESP_OK;
        }
        if (xTaskGetTickCount() - started >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

esp_err_t bsp_net_tcp_peers(bsp_net_endpoint_id_t endpoint, bsp_net_peer_t *peers,
                          size_t capacity, size_t *count) {
    if (count == NULL || (peers == NULL && capacity != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    bsp_net_init_locks();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running || endpoint >= s_endpoint_count || s_endpoints[endpoint].mode == BSP_NET_UDP) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t index = 0; index < BSP_NET_MAX_TCP_CONNECTIONS; ++index) {
        bsp_net_channel_t *channel = &s_channels[index];
        if (channel->peer.endpoint == endpoint && channel->rx != NULL &&
            (channel->fd >= 0 || xStreamBufferBytesAvailable(channel->rx) != 0)) {
            if (*count < capacity) {
                peers[*count] = channel->peer;
            }
            ++*count;
        }
    }
    xSemaphoreGive(s_lock);
    return capacity != 0 && capacity < *count ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t bsp_net_receive_udp(bsp_net_channel_t *channel, void *buf, size_t capacity,
                                     size_t *received, bsp_net_peer_t *source, bool peek, TickType_t wait) {
    if (!channel->udp_pending) {
        if (xMessageBufferReceive(channel->rx, &channel->udp_header, sizeof(channel->udp_header), wait) == 0) {
            return ESP_ERR_NOT_FINISHED;
        }
        channel->udp_pending = true;
    }
    const bsp_net_udp_header_t *header = &channel->udp_header;
    if (!peek && header->len <= capacity && header->len != 0 &&
        xMessageBufferReceive(channel->rx, buf, capacity, 0) == 0) {
        return ESP_ERR_NOT_FINISHED;
    }
    *received = header->len;
    *source = (bsp_net_peer_t){.endpoint = channel->peer.endpoint, .port = ntohs(header->address.sin_port)};
    inet_ntop(AF_INET, &header->address.sin_addr, source->ip, sizeof(source->ip));
    if (peek) {
        return ESP_OK;
    }
    if (header->len > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    channel->udp_pending = false;
    return ESP_OK;
}

static esp_err_t bsp_net_receive(bsp_net_endpoint_id_t endpoint, const bsp_net_peer_t *peer, void *buf, size_t capacity,
                                 size_t *received, bsp_net_peer_t *source, bool peek, u32 timeout_ms) {
    *received = 0;
    bsp_net_init_locks();
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = bsp_net_ticks(timeout_ms);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const u32 epoch = s_epoch;
    bsp_net_channel_t *channel = NULL;
    if (s_running && endpoint < s_endpoint_count &&
        (s_endpoints[endpoint].mode == BSP_NET_UDP) == (peer == NULL)) {
        channel = peer == NULL ? &s_channels[BSP_NET_MAX_TCP_CONNECTIONS + endpoint] : bsp_net_find_tcp(peer);
    }
    const bool valid = channel != NULL && channel->rx != NULL;
    xSemaphoreGive(s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(channel->recv_lock, 0) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool current = s_running && epoch == s_epoch;
        xSemaphoreGive(s_lock);
        if (!current) {
            return ESP_ERR_INVALID_STATE;
        }
        if (xTaskGetTickCount() - started >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    esp_err_t result = ESP_ERR_INVALID_STATE;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool current = s_running && epoch == s_epoch && channel->rx != NULL &&
                             (peer == NULL || channel->peer.connection_id == peer->connection_id);
        const bool closed = channel->fd < 0;
        StreamBufferHandle_t rx = channel->rx;
        xSemaphoreGive(s_lock);
        if (!current || (closed && xStreamBufferBytesAvailable(rx) == 0)) {
            break;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t remaining = elapsed < timeout ? timeout - elapsed : 0;
        const TickType_t poll = bsp_net_ticks(BSP_NET_POLL_MS);
        const TickType_t wait = remaining < poll ? remaining : poll;
        size_t length = 0;
        esp_err_t read_result;
        if (peer == NULL) {
            read_result = bsp_net_receive_udp(channel, buf, capacity, &length, source, peek, wait);
        } else {
            length = xStreamBufferReceive(rx, buf, capacity, wait);
            read_result = length != 0 ? ESP_OK : ESP_ERR_NOT_FINISHED;
        }
        if (!s_running) {
            break;
        }
        if (read_result != ESP_ERR_NOT_FINISHED) {
            *received = length;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_task != NULL && s_running) {
                xTaskNotifyGive(s_task);
            }
            xSemaphoreGive(s_lock);
            result = read_result;
            break;
        }
        if (xTaskGetTickCount() - started >= timeout) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
    }
    xSemaphoreGive(channel->recv_lock);
    return result;
}

esp_err_t bsp_net_tcp_recv(const bsp_net_peer_t *peer, void *buf, size_t capacity,
                           size_t *received, u32 timeout_ms) {
    if (received != NULL) {
        *received = 0;
    }
    if (peer == NULL || peer->connection_id == 0 ||
        buf == NULL || capacity == 0 || received == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const bsp_net_peer_t target = *peer;
    return bsp_net_receive(target.endpoint, &target, buf, capacity, received, NULL, false, timeout_ms);
}

esp_err_t bsp_net_udp_recv(bsp_net_endpoint_id_t endpoint, void *buf, size_t capacity,
                          size_t *received, bsp_net_peer_t *source, u32 timeout_ms) {
    if (received != NULL) {
        *received = 0;
    }
    if (source != NULL) {
        *source = (bsp_net_peer_t){0};
    }
    if ((buf == NULL && capacity != 0) || received == NULL || source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return bsp_net_receive(endpoint, NULL, buf, capacity, received, source, false, timeout_ms);
}

esp_err_t bsp_net_udp_peek(bsp_net_endpoint_id_t endpoint, size_t *length,
                          bsp_net_peer_t *source, u32 timeout_ms) {
    if (length != NULL) {
        *length = 0;
    }
    if (source != NULL) {
        *source = (bsp_net_peer_t){0};
    }
    if (length == NULL || source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return bsp_net_receive(endpoint, NULL, NULL, 0, length, source, true, timeout_ms);
}

#else

esp_err_t bsp_net_start(const bsp_net_config_t *configs, size_t count) {
    (void)configs;
    (void)count;
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_net_stop(void) {
}

bool bsp_net_running(void) {
    return false;
}

esp_err_t bsp_net_send(const bsp_net_peer_t *peer, const void *data, size_t len, u32 timeout_ms) {
    (void)peer;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_tcp_peers(bsp_net_endpoint_id_t endpoint, bsp_net_peer_t *peers,
                          size_t capacity, size_t *count) {
    (void)endpoint;
    (void)peers;
    (void)capacity;
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_tcp_recv(const bsp_net_peer_t *peer, void *buf, size_t capacity,
                           size_t *received, u32 timeout_ms) {
    (void)peer;
    (void)buf;
    (void)capacity;
    (void)timeout_ms;
    if (received != NULL) {
        *received = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_udp_recv(bsp_net_endpoint_id_t endpoint, void *buf, size_t capacity,
                          size_t *received, bsp_net_peer_t *source, u32 timeout_ms) {
    (void)endpoint;
    (void)buf;
    (void)capacity;
    (void)timeout_ms;
    if (received != NULL) {
        *received = 0;
    }
    if (source != NULL) {
        *source = (bsp_net_peer_t){0};
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_net_udp_peek(bsp_net_endpoint_id_t endpoint, size_t *length,
                          bsp_net_peer_t *source, u32 timeout_ms) {
    (void)endpoint;
    (void)timeout_ms;
    if (length != NULL) {
        *length = 0;
    }
    if (source != NULL) {
        *source = (bsp_net_peer_t){0};
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
