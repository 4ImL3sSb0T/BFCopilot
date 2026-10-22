#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lib/tools/common_def.h"

/*
 * Wi-Fi 链路的原始数据收发（com 模块）。
 *
 * 这一层不做任何帧格式约定，只把网络对端当作一条字节流：
 *   - TCP 服务端：监听端口，接受上位机连接；收到的数据进入同一个接收缓冲，
 *     bsp_net_send() 发给所有已连接客户端（最多 BSP_NET_MAX_TCP_CLIENTS 个）。
 *   - UDP 服务端：绑定本地端口，记住最近一次发送方的地址，回复默认发给它；
 *     也可以用 bsp_net_udp_set_peer() 固定对端。
 *   - TCP 与 UDP 可以同时开启，共用同一个接收缓冲。
 *
 * 使用前提：先 bsp_wifi_init() + bsp_wifi_connect() 拿到 IP。
 *
 * 注意：
 *   - 接收缓冲是单一字节流，多连接/多来源的数据按到达顺序混在一起；台架链路
 *     只有一个对端时够用，需要区分来源请在上层加帧头。
 *   - UDP 的数据报边界不会保留：一次 recv 可能读到多个数据报，也可能读到半个。
 *   - 本层不负责重连 Wi-Fi，链路断了 bsp_net_send() 会失败，由 bsp_wifi 自己恢复。
 */

#define BSP_NET_DEFAULT_TCP_PORT 3333
#define BSP_NET_DEFAULT_UDP_PORT 3334

#define BSP_NET_MAX_TCP_CLIENTS 4
#define BSP_NET_RX_BUFFER_SIZE 4096

/// @brief 启动 TCP 服务端并开始监听（重复调用返回 ESP_ERR_INVALID_STATE）
/// @param port 监听端口，不能为 0
/// @return 未启用 Wi-Fi 返回 ESP_ERR_NOT_SUPPORTED；Wi-Fi 未连上返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_net_tcp_server_start(u16 port);

/// @brief 启动 UDP 服务端并绑定端口（可与 TCP 同时开启）
/// @param port 本地端口，不能为 0
esp_err_t bsp_net_udp_start(u16 port);

/// @brief 固定 UDP 回复地址；不调用时回复最近一次收到数据的来源
/// @param ip 点分十进制 IPv4，例如 "192.168.137.1"
esp_err_t bsp_net_udp_set_peer(const char *ip, u16 port);

/// @brief 发送一段数据：TCP 发给所有客户端，UDP 发给当前对端
/// @return 至少发往一个对端返回 ESP_OK；没有对端返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_net_send(const void *data, size_t len);

/// @brief 从接收缓冲取数据
/// @param timeout_ms 等待时间，0 表示不等待
/// @return 实际读到的字节数；0 表示超时；-1 表示参数或状态错误
int bsp_net_recv(void *buf, size_t len, u32 timeout_ms);

/// @brief 当前 TCP 客户端数量
size_t bsp_net_tcp_client_count(void);

/// @brief 是否有 TCP 服务端或 UDP 服务端在运行
bool bsp_net_running(void);

/// @brief 关闭所有 socket 并释放接收缓冲（可再次 start）
void bsp_net_stop(void);
