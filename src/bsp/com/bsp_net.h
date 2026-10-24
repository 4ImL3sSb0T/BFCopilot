#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lib/tools/common_def.h"

#define BSP_NET_DEFAULT_TCP_PORT 3333
#define BSP_NET_DEFAULT_UDP_PORT 3334
#define BSP_NET_MAX_ENDPOINTS 4
#define BSP_NET_MAX_TCP_CONNECTIONS 4
#define BSP_NET_MAX_PAYLOAD 1024
#define BSP_NET_RX_BUFFER_SIZE 4096
#define BSP_NET_TX_QUEUE_DEPTH 4
#define BSP_NET_TCP_SEND_TIMEOUT_MS 200
#define BSP_NET_CONNECT_TIMEOUT_MS 1000
#define BSP_NET_RECONNECT_MS 2000

typedef enum {
    BSP_NET_TCP_SERVER = 1,
    BSP_NET_TCP_CLIENT,
    BSP_NET_UDP,
} bsp_net_mode_t;

typedef u8 bsp_net_endpoint_id_t;

typedef struct {
    bsp_net_mode_t mode;
    u16 local_port;
    const char *remote_host;
    u16 remote_port;
} bsp_net_config_t;

typedef struct {
    bsp_net_endpoint_id_t endpoint;
    u32 connection_id;
    char ip[16];
    u16 port;
    bool connected;
} bsp_net_peer_t;

typedef struct {
    bsp_net_peer_t peer;
    size_t len;
    u8 data[BSP_NET_MAX_PAYLOAD];
} bsp_net_datagram_t;

/// @brief 同时启动多个端口，endpoint 为配置数组下标；最多 BSP_NET_MAX_ENDPOINTS 个
/// @note TCP 服务端/UDP 的 local_port 必须非零；TCP 客户端为 0 时由协议栈分配本地端口
/// @note 仅 TCP 客户端填写 remote_host/remote_port；主机名启动时解析一次，之后异步连接并自动重连
/// @note 需要 Wi-Fi 已连接；配置启动后无需保留；重复启动返回 ESP_ERR_INVALID_STATE
/// @note 所有 TCP 端口共享 BSP_NET_MAX_TCP_CONNECTIONS 个连接槽，客户端各预留一个
esp_err_t bsp_net_start(const bsp_net_config_t *configs, size_t count);

/// @brief 取消等待，丢弃待发请求，关闭 socket 并释放 RTOS 容器，可再次 start
/// @note start/stop 串行执行；并发收发会在资源释放前退出
void bsp_net_stop(void);

bool bsp_net_running(void);

/// @brief 将数据复制进指定链路的发送队列，由网络任务统一发送，不跨协议复制
/// @param peer TCP 使用 tcp_peers 返回的会话；UDP 显式填写 endpoint、ip、port，connection_id 为 0
/// @param timeout_ms 等待队列空间的时间，0 为不等待；stop 会取消等待
/// @note ESP_OK 仅表示已入队，返回后可复用原缓冲；队列满返回 ESP_ERR_TIMEOUT
/// @note 每次最多 BSP_NET_MAX_PAYLOAD 字节；TCP 不接受空数据，UDP 支持空数据报
/// @note TCP 短写续发，单个请求超过 BSP_NET_TCP_SEND_TIMEOUT_MS 未完成则断开连接
/// @note 断线丢弃该连接待发请求，不自动重放；UDP 只尝试发送一次，拥塞丢弃
esp_err_t bsp_net_send(const bsp_net_peer_t *peer, const void *data, size_t len, u32 timeout_ms);

/// @brief 查询 TCP 会话快照，包括已断开但仍有未读数据的会话
/// @param count 返回会话总数；capacity 为 0 时可只查询数量，否则不足返回 ESP_ERR_INVALID_SIZE
/// @note connected 表示快照时的连接状态；重连使用新的 connection_id，旧会话不能误发到新连接
esp_err_t bsp_net_tcp_peers(bsp_net_endpoint_id_t endpoint, bsp_net_peer_t *peers,
                          size_t capacity, size_t *count);

/// @brief 从指定 TCP 会话的独立 StreamBuffer 读取字节流，不保留发送调用的边界
/// @note 缓冲满时暂停读取 socket 施加背压；断线后允许读完剩余数据，读完才复用内部槽位
/// @return ESP_OK 表示 received > 0；ESP_ERR_TIMEOUT 表示暂无数据；会话失效/停止返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_net_tcp_recv(const bsp_net_peer_t *peer, void *buf, size_t capacity,
                           size_t *received, u32 timeout_ms);

/// @brief 从指定端口的 MessageBuffer 取出一个完整 UDP 数据报，包括实际来源和长度；支持空报文
/// @note 超长包、接收容器满时整包丢弃；不执行用户回调，业务任务自行处理收到的报文
/// @return ESP_OK 表示取到一包；ESP_ERR_TIMEOUT 表示暂无报文；未启用/停止返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_net_udp_recv(bsp_net_endpoint_id_t endpoint, bsp_net_datagram_t *datagram, u32 timeout_ms);
