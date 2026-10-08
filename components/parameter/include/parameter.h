#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parameter store.
 *
 * Conventions that the PLAN requires and this module enforces:
 *   - Everything uncertain is a parameter, never a hardcoded limit. Fields whose
 *     values are not yet measured carry calibrated == 0 and their numeric values are
 *     placeholders, not flight limits.
 *   - Persistence is a single CRC-protected blob written to one of two NVS slots (A/B)
 *     with an active-slot pointer, so a power loss mid-write can never leave the boot
 *     path without a usable record.
 *   - Writes are range-checked against the descriptor table before they are accepted.
 */

#define BF_PARAM_SCHEMA_VERSION 2u
#define BF_PARAM_SERVO_COUNT 2

typedef enum {
    BF_SERVO_LEFT = 0,
    BF_SERVO_RIGHT = 1,
} bf_servo_id_t;

/* Per-servo calibration. Until `calibrated` is set, none of the pulse values may be
 * trusted and the servo layer must refuse to emit a pulse. */
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

typedef struct {
    uint8_t calibrated;
    int32_t vbat_warn_mv;
    int32_t vbat_crit_mv;
    int32_t vbat_max_mv;
    int32_t iservo_max_ma; /* per-servo current limit */
} bf_safety_cfg_t;

typedef struct {
    uint8_t calibrated;
    uint16_t vbat_div_num;      /* battery divider ratio numerator */
    uint16_t vbat_div_den;      /* battery divider ratio denominator */
    uint16_t iservo_shunt_mohm; /* shunt resistance in milliohms */
    uint16_t ina199_gain;       /* INA199 gain, V/V */
    int32_t iservo_zero_mv;     /* INA199 output at zero current, must be measured */
} bf_sensors_cfg_t;

typedef struct {
    uint32_t control_period_us; /* servo/control tick period */
    uint32_t telemetry_period_ms;
    float max_flapping_hz;
    float ramp_s;
} bf_system_cfg_t;

typedef struct {
    uint32_t schema_version;
    bf_servo_cal_t servo[BF_PARAM_SERVO_COUNT];
    bf_safety_cfg_t safety;
    bf_sensors_cfg_t sensors;
    bf_system_cfg_t system;
} bf_params_t;

typedef enum {
    BF_PT_U8 = 0,
    BF_PT_I8,
    BF_PT_U16,
    BF_PT_I16,
    BF_PT_U32,
    BF_PT_I32,
    BF_PT_F32,
} bf_param_type_t;

/* Identifiers index the descriptor table and must stay in sync with it. */
typedef enum {
    BF_PARAM_SERVO_L_DIRECTION = 0,
    BF_PARAM_SERVO_L_CALIBRATED,
    BF_PARAM_SERVO_L_ZERO_US,
    BF_PARAM_SERVO_L_ANGLE_MIN_US,
    BF_PARAM_SERVO_L_ANGLE_MAX_US,
    BF_PARAM_SERVO_L_ANGLE_MIN_DEG,
    BF_PARAM_SERVO_L_ANGLE_MAX_DEG,
    BF_PARAM_SERVO_L_CENTER_DEG,
    BF_PARAM_SERVO_L_SAFE_DEG,
    BF_PARAM_SERVO_L_MAX_SPEED_DPS,
    BF_PARAM_SERVO_R_DIRECTION,
    BF_PARAM_SERVO_R_CALIBRATED,
    BF_PARAM_SERVO_R_ZERO_US,
    BF_PARAM_SERVO_R_ANGLE_MIN_US,
    BF_PARAM_SERVO_R_ANGLE_MAX_US,
    BF_PARAM_SERVO_R_ANGLE_MIN_DEG,
    BF_PARAM_SERVO_R_ANGLE_MAX_DEG,
    BF_PARAM_SERVO_R_CENTER_DEG,
    BF_PARAM_SERVO_R_SAFE_DEG,
    BF_PARAM_SERVO_R_MAX_SPEED_DPS,
    BF_PARAM_SAFETY_CALIBRATED,
    BF_PARAM_SAFETY_VBAT_WARN_MV,
    BF_PARAM_SAFETY_VBAT_CRIT_MV,
    BF_PARAM_SAFETY_VBAT_MAX_MV,
    BF_PARAM_SAFETY_ISERVO_MAX_MA,
    BF_PARAM_SENSORS_CALIBRATED,
    BF_PARAM_SENSORS_VBAT_DIV_NUM,
    BF_PARAM_SENSORS_VBAT_DIV_DEN,
    BF_PARAM_SENSORS_ISERVO_SHUNT_MOHM,
    BF_PARAM_SENSORS_INA199_GAIN,
    BF_PARAM_SENSORS_ISERVO_ZERO_MV,
    BF_PARAM_SYSTEM_CONTROL_PERIOD_US,
    BF_PARAM_SYSTEM_TELEMETRY_PERIOD_MS,
    BF_PARAM_SERVO_L_MAX_ACCELERATION,
    BF_PARAM_SERVO_R_MAX_ACCELERATION,
    BF_PARAM_SYSTEM_MAX_FLAPPING_HZ,
    BF_PARAM_SYSTEM_RAMP_S,
    BF_PARAM_COUNT,
} bf_param_id_t;

typedef struct {
    bf_param_id_t id;
    const char *name;
    const char *unit;
    bf_param_type_t type;
    uint16_t offset;
    double min;
    double max;
    bool calibration; /* true = calibration/safety config, only writable while locked */
} bf_param_desc_t;

/* Load parameters from NVS. Always succeeds if it can proceed with defaults; the
 * caller should inspect parameter_status() to learn whether stored data was rejected. */
esp_err_t parameter_init(void);

/* True when the active record loaded cleanly. False means defaults are in effect and a
 * PARAM_CORRUPT condition should be reported. */
bool parameter_is_valid(void);

/* Snapshot accessor; the returned pointer is stable for the process lifetime. */
const bf_params_t *parameter_get(void);

/* Direct struct update followed by validation + persist. Prefer the id-based setter
 * for individual scalars. */
esp_err_t parameter_commit(const bf_params_t *next);

esp_err_t parameter_reset_to_defaults(void);

/* Descriptor / scalar access used by the console and tests. */
size_t parameter_count(void);
const bf_param_desc_t *parameter_descriptor(size_t index);
const bf_param_desc_t *parameter_find(const char *name);
esp_err_t parameter_get_scalar(bf_param_id_t id, double *out);
esp_err_t parameter_set_scalar(bf_param_id_t id, double value);

#ifdef __cplusplus
}
#endif
