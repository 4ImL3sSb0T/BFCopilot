#pragma once

#include "lib/tools/common_def.h"
#include <stdbool.h>

/*
 * Wi-Fi STA 链路（com 模块）。
 *
 * 凭据来自 menuconfig：CONFIG_BF_WIFI_SSID / CONFIG_BF_WIFI_PASSWORD
 * （src/bsp/Kconfig）。凭据只在运行时通过 esp_wifi_set_config 传入，
 * 并用 WIFI_STORAGE_RAM 保存，不写进 NVS。
 *
 * 默认由 CONFIG_BF_WIFI_ENABLED=n 关闭；关闭时所有 API 返回
 * EXIT_NOT_SUPPORTED，不启动射频。
 */

typedef struct {
    bool connected;
    char ip[16];        // 点分十进制，未连接时为空串
} bsp_wifi_status_t;

/// @brief 初始化 NVS / netif / 事件循环 / Wi-Fi STA 并写入凭据
/// @return 未启用 Wi-Fi 或未配置 SSID 时返回 EXIT_NOT_SUPPORTED / EXIT_INVALID_PARAM
exit_code_t bsp_wifi_init(void);

/// @brief 启动 Wi-Fi 并连接，等待拿到 IP
/// @param timeout_ms 等待超时；实际调用期间断开会自动重连
/// @return EXIT_OK 已连接；EXIT_TIMEOUT 超时；EXIT_FAIL 失败
exit_code_t bsp_wifi_connect(u32 timeout_ms);

/// @brief 主动断开并停止自动重连
void bsp_wifi_disconnect(void);

/// @brief 当前是否已拿到 IP
bool bsp_wifi_is_connected(void);

/// @brief 读取连接状态与 IP
exit_code_t bsp_wifi_get_status(bsp_wifi_status_t *status);
