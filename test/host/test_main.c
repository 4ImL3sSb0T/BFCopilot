/* Host-side unit tests for the pure logic that has no ESP-IDF dependency.
 *
 * Build (from this directory):
 *     clang test_main.c ../../src/bsp/common/bf_crc.c ../../src/bsp/common/bf_fault.c \
 *           -I../../src/bsp/common/include -I../../src/driver/servo/include -o bf_host_tests
 * or with CMake: cmake -B build && cmake --build build && ctest --test-dir build
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bf_crc.h"
#include "bf_fault.h"
#include "servo_math.h"
#include "flapping.h"
#include "bf_protocol.h"

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
    CHECK(!servo_map_angle_to_pulse_us(NAN, +1, -90, 90, 1000, 2000, &us), "NaN rejects");
    CHECK(!servo_map_angle_to_pulse_us(0, 0, -90, 90, 1000, 2000, &us), "bad direction rejects");
    CHECK(!servo_map_angle_to_pulse_us(0, 1, -90, 90, 1000, 2000, NULL), "null rejects");
}

static void test_flapping(void)
{
    flapping_limits_t limits = { .min_deg = {-30, -30}, .max_deg = {30, 30},
        .speed_dps = {100, 100}, .acceleration_dps2 = {1000, 1000},
        .max_frequency_hz = 5, .ramp_s = 0.1f };
    flapping_command_t command = { .frequency_hz = 1, .amplitude_deg = {10, 10},
        .phase_offset_deg = 180, .mode = 2 };
    flapping_t state = {0};
    CHECK(flapping_command(&state, &command, &limits), "valid trajectory");
    for (unsigned tick = 0; tick < 10000; tick++) {
        float output[2];
        CHECK(flapping_step(&state, &limits, 0.003003f, output), "trajectory step");
        CHECK(fabsf(output[0]) <= 10.001f && fabsf(output[1]) <= 10.001f, "trajectory envelope");
        CHECK(fabsf(output[0] + output[1]) < 0.001f, "opposite phase");
    }
    float phase = state.phase_rad;
    command.frequency_hz = 0.5f;
    CHECK(flapping_command(&state, &command, &limits) && state.phase_rad == phase, "phase continuity");
    command.amplitude_deg[0] = 31;
    CHECK(!flapping_validate(&command, &limits), "travel rejects");
    command.amplitude_deg[0] = 10;
    command.frequency_hz = 2;
    CHECK(!flapping_validate(&command, &limits), "peak speed rejects");
    command.frequency_hz = 1;
    limits.acceleration_dps2[0] = 1;
    CHECK(!flapping_validate(&command, &limits), "peak acceleration rejects");
    limits.acceleration_dps2[0] = 1000;
    command.frequency_hz = NAN;
    CHECK(!flapping_validate(&command, &limits), "NaN frequency rejects");
    command.frequency_hz = 1;
    command.differential = 2;
    CHECK(!flapping_validate(&command, &limits), "differential rejects");
    float output[2];
    CHECK(!flapping_step(&state, &limits, 0.1f, output), "missed deadline rejects");
    flapping_reset(&state);
    CHECK(!flapping_step(&state, &limits, 0.003f, output), "reset disables trajectory");
}

static void test_protocol(void)
{
    bf_frame_t frame = { .type = BF_CONTROL, .session = 123, .sequence = 456 };
    bf_put_u16(frame.payload + 2, 100);
    bf_put_u32(frame.payload + 8, 789);
    uint8_t raw[BF_FRAME_SIZE];
    uint8_t encoded[BF_FRAME_SIZE + 1];
    uint8_t decoded[BF_FRAME_SIZE];
    bf_frame_encode(&frame, raw);
    bf_frame_t restored;
    CHECK(bf_frame_decode(raw, sizeof(raw), &restored), "wire roundtrip");
    CHECK(restored.session == 123 && restored.sequence == 456, "wire integers");
    size_t length = bf_cobs_encode(raw, sizeof(raw), encoded, sizeof(encoded));
    CHECK(length == 61 && bf_cobs_decode(encoded, length, decoded, sizeof(decoded)) == 60 &&
        memcmp(raw, decoded, 60) == 0, "COBS roundtrip");
    CHECK(!bf_frame_decode(raw, 59, &restored), "truncation rejects");
    for (unsigned offset = 0; offset < BF_FRAME_SIZE; offset++) {
        raw[offset] ^= 1;
        CHECK(!bf_frame_decode(raw, sizeof(raw), &restored), "corruption rejects");
        raw[offset] ^= 1;
    }
    CHECK(!bf_cobs_decode((const uint8_t *)"\x05\x01", 2, decoded, sizeof(decoded)), "COBS overflow rejects");
    flapping_command_t command;
    uint16_t remaining;
    uint32_t lease;
    CHECK(bf_control_decode(&frame, &command, &remaining, &lease) && remaining == 100 && lease == 789,
        "control decode");
    bf_put_u16(frame.payload + 4, 100);
    CHECK(!bf_control_decode(&frame, &command, &remaining, &lease), "expired upstream rejects");
    bf_put_u16(frame.payload + 4, 0);
    frame.payload[1] = 1;
    CHECK(!bf_control_decode(&frame, &command, &remaining, &lease), "reserved rejects");
    uint8_t block[508];
    uint8_t packed[511];
    uint8_t unpacked[508];
    memset(block, 1, sizeof(block));
    length = bf_cobs_encode(block, sizeof(block), packed, sizeof(packed));
    CHECK(length == 511 && bf_cobs_decode(packed, length, unpacked, sizeof(unpacked)) == 508 &&
        memcmp(block, unpacked, sizeof(block)) == 0, "COBS 254-byte boundary");
}

int main(void)
{
    test_crc();
    test_fault_mask();
    test_servo_mapping();
    test_flapping();
    test_protocol();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
