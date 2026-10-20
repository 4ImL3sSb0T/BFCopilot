#include "spa06_math.h"

#include <math.h>

i32 spa06_signed(u32 value, unsigned bits) {
    const u32 sign = 1u << (bits - 1);
    return (i32)(value & (sign - 1)) - (i32)(value & sign);
}

static i32 signed_be24(const u8 *raw) {
    return spa06_signed(((u32)raw[0] << 16) | ((u32)raw[1] << 8) | raw[2], 24);
}

bool spa06_decode_coefficients(const u8 raw[21], spa06_coefficients_t *coefficients) {
    bool all_zero = true;
    bool all_ones = true;
    for (unsigned index = 0; index < 21; index++) {
        all_zero &= raw[index] == 0;
        all_ones &= raw[index] == 0xff;
    }
    if (all_zero || all_ones) {
        return false;
    }

    coefficients->c0 = spa06_signed(((u32)raw[0] << 4) | (raw[1] >> 4), 12);
    coefficients->c1 = spa06_signed(((u32)(raw[1] & 15) << 8) | raw[2], 12);
    coefficients->c00 = spa06_signed(((u32)raw[3] << 12) |
                                     ((u32)raw[4] << 4) | (raw[5] >> 4), 20);
    coefficients->c10 = spa06_signed(((u32)(raw[5] & 15) << 16) |
                                     ((u32)raw[6] << 8) | raw[7], 20);
    coefficients->c01 = spa06_signed(((u32)raw[8] << 8) | raw[9], 16);
    coefficients->c11 = spa06_signed(((u32)raw[10] << 8) | raw[11], 16);
    coefficients->c20 = spa06_signed(((u32)raw[12] << 8) | raw[13], 16);
    coefficients->c21 = spa06_signed(((u32)raw[14] << 8) | raw[15], 16);
    coefficients->c30 = spa06_signed(((u32)raw[16] << 8) | raw[17], 16);
    coefficients->c31 = spa06_signed(((u32)raw[18] << 4) | (raw[19] >> 4), 12);
    coefficients->c40 = spa06_signed(((u32)(raw[19] & 15) << 8) | raw[20], 12);
    return true;
}

bool spa06_compensate(const spa06_coefficients_t *coefficients, const u8 raw[6],
                      f32 *pressure_pa, f32 *temperature_c) {
    // 原始值按过采样归一化：压力 8x / 温度 4x（与 bsp_baro.c 的配置一致）
    const f32 pressure_scaled = signed_be24(raw) / 7864320.0f;
    const f32 temperature_scaled = signed_be24(raw + 3) / 3670016.0f;

    const f32 temperature = coefficients->c0 * 0.5f + coefficients->c1 * temperature_scaled;
    const f32 pressure = coefficients->c00 + pressure_scaled *
        (coefficients->c10 + pressure_scaled * (coefficients->c20 + pressure_scaled *
        (coefficients->c30 + pressure_scaled * coefficients->c40))) + temperature_scaled *
        (coefficients->c01 + pressure_scaled * (coefficients->c11 + pressure_scaled *
        (coefficients->c21 + pressure_scaled * coefficients->c31)));

    if (!isfinite(pressure) || !isfinite(temperature) ||
        pressure < 30000.0f || pressure > 110000.0f ||
        temperature < -40.0f || temperature > 85.0f) {
        return false;
    }

    *pressure_pa = pressure;
    *temperature_c = temperature;
    return true;
}
