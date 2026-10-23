#pragma once

/*
 * 直连 Wi-Fi/UDP 控制链路（台架用）。
 *
 * 飞控作为 AP，电脑连上热点后每个 UDP 数据报承载一个 60 字节控制帧。
 * 这是传输层；会话、序号、TTL、lease 和 arm 校验仍由 flight 服务完成，
 * 本驱动只负责收包、回包和发现应答。默认关闭，并与 ESP-NOW 互斥。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BF_WIFI_LINK_FRAME_SIZE 60u

typedef void (*bf_wifi_link_receiver_t)(const uint8_t *data, size_t length,
                                        int64_t received_us);

esp_err_t bf_wifi_link_init(bf_wifi_link_receiver_t receiver);
esp_err_t bf_wifi_link_send(const uint8_t *data, size_t length);
bool bf_wifi_link_ready(void);

#ifdef __cplusplus
}
#endif
