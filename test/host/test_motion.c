#include <math.h>
#include <stdio.h>
#include <string.h>

#include "motion_math.h"

static int failures;

#define CHECK(condition) do { if (!(condition)) { \
    printf("FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static void test_signed(void)
{
    CHECK(motion_signed(0x7ff, 12) == 2047);
    CHECK(motion_signed(0x800, 12) == -2048);
    CHECK(motion_signed(0xfff, 12) == -1);
    CHECK(motion_signed(0x80000, 20) == -524288);
    CHECK(motion_signed(0x7fffff, 24) == 8388607);
    CHECK(motion_signed(0x800000, 24) == -8388608);
    CHECK(motion_signed(0xffffff, 24) == -1);
}

static void test_coefficients(void)
{
    uint8_t packed[21] = {0x80, 0x07, 0xff, 0x80, 0x00, 0x07, 0xff, 0xff,
                          0x80, 0x00, 0x7f, 0xff, 0xff, 0xff, 0x00, 0x01,
                          0xff, 0xfe, 0x80, 0x07, 0xff};
    spa06_coefficients_t coefficients;
    CHECK(spa06_decode_coefficients(packed, &coefficients));
    CHECK(coefficients.c0 == -2048 && coefficients.c1 == 2047);
    CHECK(coefficients.c00 == -524288 && coefficients.c10 == 524287);
    CHECK(coefficients.c01 == -32768 && coefficients.c11 == 32767);
    CHECK(coefficients.c20 == -1 && coefficients.c21 == 1 && coefficients.c30 == -2);
    CHECK(coefficients.c31 == -2048 && coefficients.c40 == 2047);
    memset(packed, 0, sizeof(packed));
    CHECK(!spa06_decode_coefficients(packed, &coefficients));
    memset(packed, 0xff, sizeof(packed));
    CHECK(!spa06_decode_coefficients(packed, &coefficients));
}

static void test_compensation(void)
{
    spa06_coefficients_t coefficients = {
        .c0 = 40, .c1 = 2, .c00 = 100000, .c10 = 1000, .c01 = 100,
        .c11 = 200, .c20 = 300, .c21 = 400, .c30 = 500, .c31 = 600, .c40 = 700,
    };
    uint8_t raw[6] = {0x78, 0x00, 0x00, 0x38, 0x00, 0x00};
    int32_t pressure_pa = 0, temperature_mdegc = 0;
    CHECK(spa06_compensate(&coefficients, raw, &pressure_pa, &temperature_mdegc));
    CHECK(pressure_pa == 103800 && temperature_mdegc == 22000);
    raw[0] = 0x88;
    CHECK(spa06_compensate(&coefficients, raw, &pressure_pa, &temperature_mdegc));
    CHECK(pressure_pa == 99200);
    raw[3] = 0xc8;
    CHECK(spa06_compensate(&coefficients, raw, &pressure_pa, &temperature_mdegc));
    CHECK(pressure_pa == 99800 && temperature_mdegc == 18000);
    coefficients.c00 = 200000;
    CHECK(!spa06_compensate(&coefficients, raw, &pressure_pa, &temperature_mdegc));
    coefficients.c00 = 100000;
    coefficients.c0 = 200;
    CHECK(!spa06_compensate(&coefficients, raw, &pressure_pa, &temperature_mdegc));
}

int main(void)
{
    test_signed();
    test_coefficients();
    test_compensation();
    printf("motion tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
