#include "motion_math.h"

#include <math.h>

int32_t motion_signed(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)(value & (sign - 1)) - (int32_t)(value & sign);
}

static int32_t signed_be24(const uint8_t *raw)
{
    return motion_signed(((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2], 24);
}

bool spa06_decode_coefficients(const uint8_t raw[21], spa06_coefficients_t *coefficients)
{
    bool all_zero = true;
    bool all_ones = true;
    for (unsigned index = 0; index < 21; index++) {
        all_zero &= raw[index] == 0;
        all_ones &= raw[index] == 0xff;
    }
    if (all_zero || all_ones) return false;
    coefficients->c0 = motion_signed(((uint32_t)raw[0] << 4) | (raw[1] >> 4), 12);
    coefficients->c1 = motion_signed(((uint32_t)(raw[1] & 15) << 8) | raw[2], 12);
    coefficients->c00 = motion_signed(((uint32_t)raw[3] << 12) |
                                      ((uint32_t)raw[4] << 4) | (raw[5] >> 4), 20);
    coefficients->c10 = motion_signed(((uint32_t)(raw[5] & 15) << 16) |
                                      ((uint32_t)raw[6] << 8) | raw[7], 20);
    coefficients->c01 = motion_signed(((uint32_t)raw[8] << 8) | raw[9], 16);
    coefficients->c11 = motion_signed(((uint32_t)raw[10] << 8) | raw[11], 16);
    coefficients->c20 = motion_signed(((uint32_t)raw[12] << 8) | raw[13], 16);
    coefficients->c21 = motion_signed(((uint32_t)raw[14] << 8) | raw[15], 16);
    coefficients->c30 = motion_signed(((uint32_t)raw[16] << 8) | raw[17], 16);
    coefficients->c31 = motion_signed(((uint32_t)raw[18] << 4) | (raw[19] >> 4), 12);
    coefficients->c40 = motion_signed(((uint32_t)(raw[19] & 15) << 8) | raw[20], 12);
    return true;
}

bool spa06_compensate(const spa06_coefficients_t *coefficients, const uint8_t raw[6],
                      int32_t *pressure_pa, int32_t *temperature_mdegc)
{
    float pressure_scaled = signed_be24(raw) / 7864320.0f;
    float temperature_scaled = signed_be24(raw + 3) / 3670016.0f;
    float temperature = coefficients->c0 * 0.5f + coefficients->c1 * temperature_scaled;
    float pressure = coefficients->c00 + pressure_scaled *
        (coefficients->c10 + pressure_scaled * (coefficients->c20 + pressure_scaled *
        (coefficients->c30 + pressure_scaled * coefficients->c40))) + temperature_scaled *
        (coefficients->c01 + pressure_scaled * (coefficients->c11 + pressure_scaled *
        (coefficients->c21 + pressure_scaled * coefficients->c31)));
    if (!isfinite(pressure) || !isfinite(temperature) ||
        pressure < 30000.0f || pressure > 110000.0f || temperature < -40.0f || temperature > 85.0f) {
        return false;
    }
    *pressure_pa = (int32_t)lroundf(pressure);
    *temperature_mdegc = (int32_t)lroundf(temperature * 1000.0f);
    return true;
}
