#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pure angle -> pulse-width mapping, kept free of ESP-IDF dependencies so it can be
 * unit-tested on the host. The two calibration points define a linear segment; the
 * `direction` sign mirrors the mapping so a left/right pair mounted as mirror images
 * shares one commanded-angle convention.
 *
 * Returns false when the calibration span is degenerate (max <= min), which callers
 * must treat as "not calibrated".
 */
static inline bool servo_map_angle_to_pulse_us(float deg, int8_t direction,
                                               float angle_min_deg, float angle_max_deg,
                                               uint16_t angle_min_us, uint16_t angle_max_us,
                                               float *out_us)
{
    if (angle_max_deg <= angle_min_deg) {
        return false;
    }

    float span = angle_max_deg - angle_min_deg;
    float t = (direction >= 0) ? (deg - angle_min_deg) / span
                               : (angle_max_deg - deg) / span;
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }

    *out_us = (float)angle_min_us + t * ((float)angle_max_us - (float)angle_min_us);
    return true;
}

#ifdef __cplusplus
}
#endif
