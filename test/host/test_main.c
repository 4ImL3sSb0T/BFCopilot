/* Host-side unit tests for the pure logic that has no ESP-IDF dependency.
 *
 * Build (from this directory):
 *     clang test_main.c ../../components/common/bf_crc.c ../../components/common/bf_fault.c \
 *           -I../../components/common/include -I../../components/servo/include -o bf_host_tests
 * or with CMake: cmake -B build && cmake --build build && ctest --test-dir build
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bf_crc.h"
#include "bf_fault.h"
#include "servo_math.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, fmt, ...)                                        \
    do {                                                             \
        g_checks++;                                                  \
        if (!(cond)) {                                               \
            g_failures++;                                            \
            printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                            \
    } while (0)

static void test_crc(void)
{
    CHECK(bf_crc32("", 0) == 0x00000000u, "empty CRC");
    CHECK(bf_crc32("123456789", 9) == 0xCBF43926u, "check vector 123456789");
    CHECK(bf_crc32("The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u,
          "check vector pangram");
}

static void test_fault_mask(void)
{
    char buf[64];
    CHECK(strcmp(bf_fault_mask_str(BF_FAULT_MASK_NONE, buf, sizeof(buf)), "none") == 0,
          "none mask");
    bf_fault_mask_t m = BF_FAULT_BIT(BF_FAULT_LINK_LOST) | BF_FAULT_BIT(BF_FAULT_ESTOP);
    CHECK(strcmp(bf_fault_mask_str(m, buf, sizeof(buf)), "LINK_LOST|ESTOP") == 0,
          "combo mask got '%s'", buf);

    CHECK(bf_fault_is_latching(BF_FAULT_ESTOP), "estop latches");
    CHECK(!bf_fault_is_latching(BF_FAULT_PARAM_CORRUPT), "param corrupt does not latch");
}

static bool approx(float a, float b)
{
    return fabsf(a - b) < 0.01f;
}

static void test_servo_mapping(void)
{
    float us = 0.0f;

    /* Positive direction: angle rises with pulse width. */
    CHECK(servo_map_angle_to_pulse_us(-90.0f, +1, -90.0f, 90.0f, 1000, 2000, &us) && approx(us, 1000.0f),
          "dir+ min");
    CHECK(servo_map_angle_to_pulse_us(0.0f, +1, -90.0f, 90.0f, 1000, 2000, &us) && approx(us, 1500.0f),
          "dir+ mid");
    CHECK(servo_map_angle_to_pulse_us(90.0f, +1, -90.0f, 90.0f, 1000, 2000, &us) && approx(us, 2000.0f),
          "dir+ max");

    /* Negative direction mirrors it. */
    CHECK(servo_map_angle_to_pulse_us(-90.0f, -1, -90.0f, 90.0f, 1000, 2000, &us) && approx(us, 2000.0f),
          "dir- min");
    CHECK(servo_map_angle_to_pulse_us(90.0f, -1, -90.0f, 90.0f, 1000, 2000, &us) && approx(us, 1000.0f),
          "dir- max");

    /* Out-of-range angles clamp to the endpoints. */
    CHECK(servo_map_angle_to_pulse_us(200.0f, +1, -90.0f, 90.0f, 1000, 2000, &us) && approx(us, 2000.0f),
          "clamp high");
    CHECK(servo_map_angle_to_pulse_us(-200.0f, +1, -90.0f, 90.0f, 1000, 2000, &us) && approx(us, 1000.0f),
          "clamp low");

    /* Degenerate calibration is rejected. */
    CHECK(!servo_map_angle_to_pulse_us(0.0f, +1, 0.0f, 0.0f, 1000, 2000, &us),
          "degenerate rejects");
}

int main(void)
{
    test_crc();
    test_fault_mask();
    test_servo_mapping();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
