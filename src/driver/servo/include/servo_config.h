#pragma once

#include <stdint.h>

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
