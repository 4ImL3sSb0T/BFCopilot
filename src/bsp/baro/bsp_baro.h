#pragma once

#include "lib/tools/common_def.h"
#include "esp_err.h"
#include <stdbool.h>

/*
 * 气压计：U5 SPA06-003，I2C。
 *
 * 原理图 P1（2026-10-20）：
 *   SCL=GPIO4  SDA=GPIO5
 *   CSB/SDO 悬空（NC）—— SDO 没有固定电平，所以 I2C 从地址 0x76/0x77
 *   不确定，初始化时必须两个都探测，不能写死。
 *
 * 采样配置沿用旧分支已验证的组合：连续测量，压力 8x、温度 4x 过采样。
 * 这两个过采样档位与 spa06_math.c 里的 7864320 / 3670016 缩放系数是配套的，
 * 改动其中任何一个都必须同时改另一个。
 * 系数从 0x10 起 21 字节读出后做二阶补偿（见 spa06_math.c）。
 */

#define BSP_BARO_PIN_SCL            4
#define BSP_BARO_PIN_SDA            5
#define BSP_BARO_I2C_CLOCK_HZ       100000
#define BSP_BARO_I2C_TIMEOUT_MS     20

#define BSP_BARO_ADDR_PRIMARY       0x77
#define BSP_BARO_ADDR_SECONDARY     0x76
#define BSP_BARO_CHIP_ID            0x11

typedef struct {
    f32 pressure_pa;      // 补偿后的气压，单位 Pa
    f32 temperature_c;    // 补偿后的温度，单位 °C
} bsp_baro_sample_t;

/// @brief 初始化 I2C、探测地址、读系数并配置连续测量
/// @note 两个候选地址都探测；ID 不符或系数异常时返回 ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_RESPONSE
esp_err_t bsp_baro_init(void);

/// @brief 返回气压计是否已成功初始化
bool bsp_baro_is_ready(void);

/// @brief 返回实际探测到的 7 位从地址（未初始化返回 0）
u8 bsp_baro_address(void);

/// @brief 读一帧补偿后的气压/温度
/// @return ESP_OK 有新气压数据；ESP_ERR_NOT_FINISHED 本周期无新数据（sample 不变）
esp_err_t bsp_baro_read(bsp_baro_sample_t *sample);
