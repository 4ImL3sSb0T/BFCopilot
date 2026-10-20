#pragma once

#include "lib/tools/common_def.h"
#include <stdbool.h>

/*
 * SPA06-003 系数解码与二阶补偿。
 *
 * 纯计算，不依赖 ESP-IDF，方便在主机上单独验证。寄存器语义和位宽来自
 * SPA06-003 数据手册；raw[0..2] 是压力原始值（24 位有符号，大端），
 * raw[3..5] 是温度原始值。
 */

typedef struct {
    i32 c0, c1, c00, c10, c01, c11, c20, c21, c30, c31, c40;
} spa06_coefficients_t;

/// @brief 把 bits 位有符号补码转成 i32
i32 spa06_signed(u32 value, unsigned bits);

/// @brief 解码 0x10 起 21 字节系数
/// @return 全 0 或全 0xFF（器件未校准/读错）时返回 false
bool spa06_decode_coefficients(const u8 raw[21], spa06_coefficients_t *coefficients);

/// @brief 用系数补偿一帧原始数据
/// @param raw 6 字节：压力 3 字节 + 温度 3 字节
/// @return 结果超出合理范围（30–110 kPa / −40–85 °C）时返回 false
bool spa06_compensate(const spa06_coefficients_t *coefficients, const u8 raw[6],
                      f32 *pressure_pa, f32 *temperature_c);
