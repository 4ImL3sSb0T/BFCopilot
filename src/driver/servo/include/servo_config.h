#pragma once

#include <stdint.h>

/*
 * Servo fitted to the current airframe: PTK 7485 Plus HV (coreless, metal gears,
 * digital). The figures below are the nameplate specification - electrical limits and
 * nominal performance. They are NOT a calibration: they say what the servo accepts,
 * not where the linkage actually sits on a built wing pair.
 *
 * They are safe to use as parameter defaults because `calibrated = 0` keeps them out of
 * every control path (arming, servo_enable(), flapping all refuse uncalibrated sides).
 * The bench still has to measure per side: mechanical zero, the usable travel after the
 * linkage is fitted, direction, and loaded speed.
 */
#define SERVO_SPEC_NAME              "PTK 7485 Plus HV"
#define SERVO_SPEC_FRAME_HZ          333u    /* max refresh rate; the project outputs 200 Hz */
#define SERVO_SPEC_PULSE_MIN_US      500u    /* full-travel electrical endpoints */
#define SERVO_SPEC_PULSE_MAX_US      2500u
#define SERVO_SPEC_CENTER_US         1520u   /* factory neutral pulse */
#define SERVO_SPEC_DEADBAND_US       3u
#define SERVO_SPEC_TRAVEL_DEG        180.0f  /* nominal, tolerance +/-10 deg */
#define SERVO_SPEC_RATE_DPS_74V      857.0f  /* 60 deg / 0.070 s at 7.4 V, no load */
#define SERVO_SPEC_VOLTAGE_MIN_MV    5000    /* 5.0 V - 8.4 V, 2S capable */
#define SERVO_SPEC_VOLTAGE_MAX_MV    8400
#define SERVO_SPEC_IDLE_MA_74V       56      /* no-load running current, not a limit */
#define SERVO_SPEC_STALL_TORQUE_MNM  735     /* 7.5 kg*cm at 7.4 V */

typedef enum {
    BF_SERVO_LEFT = 0,
    BF_SERVO_RIGHT = 1,
} bf_servo_id_t;

typedef struct {
    int8_t direction;      /* +1 or -1, sign of angle -> pulse slope */
    uint8_t calibrated;    /* 0 = uncalibrated placeholder */
    uint16_t zero_us;      /* pulse width at zero_us angle datum (servo mechanical zero) */
    uint16_t angle_min_us; /* pulse width at angle_min_deg */
    uint16_t angle_max_us; /* pulse width at angle_max_deg */
    float angle_min_deg;
    float angle_max_deg;
    float center_deg;
    float safe_deg;        /* angle to return to on link loss / estop */
    float max_speed_dps;   /* slew limit, degrees per second */
    float max_acceleration_dps2;
} bf_servo_cal_t;
