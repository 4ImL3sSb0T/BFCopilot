#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dual-servo output. Two MCPWM generators share a single 333 Hz time base so the left
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

/* Nominal frame period. The PLAN fixes 333 Hz; this is a bench/electrical constant,
 * not a flight limit. */
#define SERVO_FRAME_HZ 333

/* Electrical sanity bounds for the pulse width in microseconds. These exist only so a
 * corrupt calibration cannot command an absurd pulse; they are NOT servo limits. */
#define SERVO_PULSE_SANITY_MIN_US 200
#define SERVO_PULSE_SANITY_MAX_US 2800

esp_err_t servo_init(void);

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

/* Re-read calibration from the parameter store. */
esp_err_t servo_reload_params(void);

/* Immediately stop pulses without changing angle state. */
void servo_force_output_off(void);

#ifdef __cplusplus
}
#endif
