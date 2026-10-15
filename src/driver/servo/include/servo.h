#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "servo_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dual-servo output. Two MCPWM generators share a single 50 Hz time base so the left
 * and right pulse trains are always phase-locked.
 *
 * Safety contract:
 *   - After servo_init() the outputs emit no pulse at all (generator held low).
 *   - servo_enable(true) only produces pulses when the servo's stored calibration is
 *     marked valid and the resulting pulse is inside the electrical sanity band.
 *   - Nothing here starts flapping; this module only drives a commanded angle.
 */

typedef enum {
    SERVO_LEFT = 0,
    SERVO_RIGHT = 1,
    SERVO_COUNT = 2,
} servo_idx_t;

/* Nominal frame period. The PLAN fixes 50 Hz; this is a bench/electrical constant,
 * not a flight limit. */
#define SERVO_FRAME_HZ 50

/* Electrical sanity bounds for the pulse width in microseconds. These exist only so a
 * corrupt calibration cannot command an absurd pulse; they are NOT servo limits. */
#define SERVO_PULSE_SANITY_MIN_US 200
#define SERVO_PULSE_SANITY_MAX_US 2800

esp_err_t servo_init(const bf_servo_cal_t calibration[SERVO_COUNT]);

/* Enable/disable pulse output. Disabling holds both generators low and keeps the
 * internal angle state, so re-enabling resumes from a known point. */
esp_err_t servo_enable(bool enable);
bool servo_is_enabled(void);

bool servo_calibration_valid(servo_idx_t servo);

/* Commanded angle (degrees) is clamped to the calibrated travel limits. */
esp_err_t servo_set_target_deg(servo_idx_t servo, float deg);
esp_err_t servo_get_target_deg(servo_idx_t servo, float *out);

/* Angle that has actually been commanded after slew limiting. */
esp_err_t servo_get_output_deg(servo_idx_t servo, float *out);
esp_err_t servo_get_pulse_us(servo_idx_t servo, float *out);

/* Advance slew limiting and push compare values. Call once per control tick. */
esp_err_t servo_update(float dt_s);

/* Set both targets to their calibrated safe angles (used by link-loss / estop). */
esp_err_t servo_return_to_safe(void);
esp_err_t servo_get_safe_deg(servo_idx_t servo, float *out);

/* Reset outputs using the configuration supplied at initialization. */
esp_err_t servo_reload_params(void);

/* Immediately stop pulses without changing angle state. */
void servo_force_output_off(void);

/*
 * Bench-only raw pulse control (see test/bench).
 *
 * These drive the compare value directly, bypassing the two-point calibration, so a servo
 * can be exercised before any calibration exists - which is the state a fresh board is in.
 * The pulse is clamped to the same sanity range as the normal path. `output_deg` is left
 * alone, so read the pulse back with servo_debug_get_pulse_us() rather than
 * servo_get_pulse_us(), which reports what the calibrated path would produce.
 *
 * Nothing on the flight path may call these, and the calibrated path takes the pins back
 * on the next servo_set_target_deg().
 */
esp_err_t servo_debug_set_pulse_us(servo_idx_t servo, float pulse_us);
esp_err_t servo_debug_get_pulse_us(servo_idx_t servo, float *out_us);
esp_err_t servo_debug_off(servo_idx_t servo);

/* Bench-only: jump straight to the angle the calibration maps, with no slew ramp. Returns
 * ESP_ERR_INVALID_STATE when that side is not calibrated. */
esp_err_t servo_debug_set_angle_deg(servo_idx_t servo, float deg);

#ifdef __cplusplus
}
#endif
