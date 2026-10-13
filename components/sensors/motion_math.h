#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t c0, c1, c00, c10, c01, c11, c20, c21, c30, c31, c40;
} spa06_coefficients_t;

int32_t motion_signed(uint32_t value, unsigned bits);
bool spa06_decode_coefficients(const uint8_t raw[21], spa06_coefficients_t *coefficients);
bool spa06_compensate(const spa06_coefficients_t *coefficients, const uint8_t raw[6],
                      int32_t *pressure_pa, int32_t *temperature_mdegc);
