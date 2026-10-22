#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lib/tools/common_def.h"

/*
 * Wi-Fi 链路的原始数据收发（com 模块）。
 *
 * 这一层不做任何帧格式约定，只把网络对端当作一条字节流。角色和数据去向：
 *   - TCP 服务端 bsp_net_tcp_server_start()：板子监听端口，上位机连进来；
 *     收到的数据进入同一个接收缓冲，bsp_net_send() 发给所有已连接客户端。
 *   - TCP 客户端 bsp_net_tcp_client_start()：板子主动连到 host:port，断线后每
 *     BSP_NET_RECONNECT_MS 自动重连；bsp_net_send() 发给这个服务器。
 *   - UDP 服务端 bsp_net_udp_start()：绑定本地端口，接收来自任意来源的数据；
 *     回复默认发给最后一次发送方（bsp_net_udp_set_peer() 可固定），也可以用
 *     bsp_net_udp_send_to() 每次显式指定接收方。
 *   - 以上三种可以同时开启，共用同一个接收缓冲。
 *
 * 使用前提：先 bsp_wifi_init() + bsp_wifi_connect() 拿到 IP。
 *
 * 发给谁：
 *   - TCP 服务端和“UDP 回复最近发送方”都不需要预先知道对端地址，连接建立或
 *     数据到达后本层自己记下来；
 *   - TCP 客户端和固定 UDP 对端需要调用方传入对方 IP；UDP 也可以每次用
 *     bsp_net_udp_send_to() 显式指定，或发给广播地址 "255.255.255.255"。
 *   - 想知道当前实际对端，用 bsp_net_tcp_get_peer() / bsp_net_udp_get_peer()。
 *
 * 注意：
 *   - 接收缓冲是单一字节流，多连接/多来源的数据按到达顺序混在一起；台架链路
 *     只有一个对端时够用，需要区分来源请在上层加帧头。
 *   - UDP 的数据报边界不会保留：一次 recv 可能读到多个数据报，也可能读到半个。
 *     单个 UDP 数据报接收上限为 BSP_NET_MAX_UDP_RX_SIZE；超限或接收缓冲不足时
 *     整包丢弃并记录日志，不向接收流交付截断的数据报。
 *   - 本层不负责重连 Wi-Fi，链路断了 bsp_net_send() 会失败，由 bsp_wifi 自己恢复。
 */

#define BSP_NET_DEFAULT_TCP_PORT 3333
#define BSP_NET_DEFAULT_UDP_PORT 3334

#define BSP_NET_MAX_TCP_CLIENTS 4
#define BSP_NET_RX_BUFFER_SIZE 4096
#define BSP_NET_MAX_UDP_RX_SIZE 1024
#define BSP_NET_RECONNECT_MS 2000

/// @brief 启动 TCP 服务端并开始监听（重复调用返回 ESP_ERR_INVALID_STATE）
/// @param port 监听端口，不能为 0
/// @return 未启用 Wi-Fi 返回 ESP_ERR_NOT_SUPPORTED；Wi-Fi 未连上返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_net_tcp_server_start(u16 port);

/// @brief 以客户端身份连接 TCP 服务器，断线后自动重连
/// @param host IPv4 地址或主机名（DNS 在调用时解析一次）
/// @param port 服务器端口，不能为 0
/// @note 连接建立是异步的：返回 ESP_OK 只表示已记录目标并开始尝试，可以用
///       bsp_net_tcp_client_count() 确认是否真的连上
esp_err_t bsp_net_tcp_client_start(const char *host, u16 port);

/// @brief 启动 UDP 服务端并绑定端口（可与 TCP 同时开启）
/// @param port 本地端口，不能为 0
esp_err_t bsp_net_udp_start(u16 port);

/// @brief 固定 UDP 回复地址，后续来包不会覆盖；stop 后恢复自动记录来源
/// @param ip 点分十进制 IPv4，例如 "192.168.137.1"
esp_err_t bsp_net_udp_set_peer(const char *ip, u16 port);

/// @brief 用 UDP 向指定地址发送一段数据（不改变默认回复地址）
/// @note 需要先 bsp_net_udp_start()
esp_err_t bsp_net_udp_send_to(const char *ip, u16 port, const void *data, size_t len);

/// @brief 发送一段数据
/// @note TCP 服务端发给所有客户端，TCP 客户端发给服务器，UDP 发给当前对端；
///       同一份数据会同时走所有已开启的通道
/// @return 至少发往一个对端返回 ESP_OK；没有对端返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_net_send(const void *data, size_t len);

/// @brief 从接收缓冲取数据
/// @param timeout_ms 等待时间，0 表示不等待
/// @note 多任务接收会串行化，等待接收锁计入本次超时；stop 会取消正在等待的接收
/// @return 实际读到的字节数；0 表示超时；-1 表示参数或状态错误
int bsp_net_recv(void *buf, size_t len, u32 timeout_ms);

/// @brief 当前已建立的 TCP 连接数（服务端接入的客户端 + 客户端模式）
size_t bsp_net_tcp_client_count(void);

/// @brief 按序号读取一个已建立 TCP 连接的对端地址
/// @param index 0 .. bsp_net_tcp_client_count()-1；先列服务端接入的客户端，再列客户端模式的对端
/// @param size ip 缓冲大小，建议至少 16 字节；不足时返回 ESP_ERR_INVALID_SIZE
/// @param port 可为 NULL
/// @return 越界或还没有连接返回 ESP_ERR_NOT_FOUND
esp_err_t bsp_net_tcp_get_peer(size_t index, char *ip, size_t size, u16 *port);

/// @brief 读取 UDP 当前的回复地址（固定对端或最近一次发送方）
/// @param size ip 缓冲大小，建议至少 16 字节；不足时返回 ESP_ERR_INVALID_SIZE
/// @param port 可为 NULL
/// @return 还没有对端返回 ESP_ERR_NOT_FOUND
esp_err_t bsp_net_udp_get_peer(char *ip, size_t size, u16 *port);

/// @brief 是否有 TCP 服务端、TCP 客户端或 UDP 服务端在运行
bool bsp_net_running(void);

/// @brief 关闭所有 socket 并释放接收缓冲（可再次 start）
/// @note 等待内部任务与正在执行的接收退出；可与收发并发调用，start/stop 串行执行
void bsp_net_stop(void);
