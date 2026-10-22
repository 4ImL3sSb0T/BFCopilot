#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "lib/tools/common_def.h"

/*
 * 串口链路（com 模块）。
 *
 * 默认打开 UART0（板上的 USB-UART 桥接，115200 8N1）。bsp_uart_recv() 从驱动
 * 环形缓冲取数据，因此不会和 ESP_LOG 的输出去抢 FIFO。
 *
 * 注意：UART0 同时是 console（CONFIG_ESP_CONSOLE_UART），日志和链路数据共线。
 * 需要干净的二进制链路时，请把 console 切到 USB Serial JTAG，或在接收端按帧头
 * 过滤日志。初始化后 esp_log 会走 UART 驱动，和 bsp_uart_send() 共用同一把
 * TX 锁，不会在字节层面交错。
 */

#define BSP_UART_DEFAULT_PORT 0
#define BSP_UART_DEFAULT_BAUD_RATE 115200
#define BSP_UART_DEFAULT_RX_BUFFER_SIZE 1024

typedef struct {
    u8 port;            // UART_NUM_x，默认 0
    u32 baud_rate;      // 默认 115200
    i32 tx_gpio;        // -1 表示沿用当前引脚（UART0 为启动时的默认引脚）
    i32 rx_gpio;        // -1 同上
    u16 rx_buffer_size; // 驱动接收环形缓冲，必须大于硬件 FIFO（128 字节）
    u16 tx_buffer_size; // 0 表示发送直接写 FIFO，不建软件缓冲
} bsp_uart_config_t;

#define BSP_UART_CONFIG_DEFAULT()           \
    {                                       \
        .port = BSP_UART_DEFAULT_PORT,      \
        .baud_rate = BSP_UART_DEFAULT_BAUD_RATE, \
        .tx_gpio = -1,                      \
        .rx_gpio = -1,                      \
        .rx_buffer_size = BSP_UART_DEFAULT_RX_BUFFER_SIZE, \
        .tx_buffer_size = 0,                \
    }

/// @brief 安装 UART 驱动（config 传 NULL 时用默认值：UART0 / 115200 / 8N1）
/// @return 重复初始化返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_uart_init(const bsp_uart_config_t *config);

/// @brief 删除驱动并恢复 console 的默认读写路径
esp_err_t bsp_uart_deinit(void);

/// @brief 是否已经初始化
bool bsp_uart_ready(void);

/// @brief 发送原始字节（不做换行转换）
/// @return 未初始化返回 ESP_ERR_INVALID_STATE
esp_err_t bsp_uart_send(const void *data, size_t len);

/// @brief 接收原始字节
/// @param timeout_ms 等待时间，0 表示不等待
/// @return 实际读到的字节数；0 表示超时；-1 表示参数或状态错误
int bsp_uart_recv(void *buf, size_t len, u32 timeout_ms);
