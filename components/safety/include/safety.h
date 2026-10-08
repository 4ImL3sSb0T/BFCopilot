#pragma once

#include <stdbool.h>

#include "bf_fault.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Safety state machine.
 *
 * States (from the PLAN):
 *   BOOT_LOCKED -> READY_LOCKED on a clean self-check and an established session;
 *   READY_LOCKED -> ARMED only on an explicit arm request that satisfies every
 *   condition; ARMED -> SAFE_RETURN_LOCKED on estop/link loss; ARMED -> FAULT_LATCHED
 *   on critical faults; SAFE_RETURN_LOCKED -> LOCKED once the servos reach their safe
 *   angle; LOCKED/FAULT_LATCHED -> READY_LOCKED only after the condition clears and the
 *   operator clears faults. There is no path that arms automatically.
 */

typedef enum {
    SAFETY_BOOT_LOCKED = 0,
    SAFETY_READY_LOCKED,
    SAFETY_ARMED,
    SAFETY_SAFE_RETURN_LOCKED,
    SAFETY_LOCKED,
    SAFETY_FAULT_LATCHED,
} safety_state_t;

/* Why the most recent arm request was refused. */
typedef enum {
    SAFETY_ARM_OK = 0,
    SAFETY_ARM_ERR_STATE,
    SAFETY_ARM_ERR_NO_SESSION,
    SAFETY_ARM_ERR_FAULTS,
    SAFETY_ARM_ERR_NOT_CALIBRATED,
    SAFETY_ARM_ERR_POWER,
} safety_arm_result_t;

esp_err_t safety_init(void);

/* Report the outcome of the power-on self-check. Moves BOOT_LOCKED to READY_LOCKED on
 * success, or FAULT_LATCHED on failure. */
void safety_boot_complete(bool self_check_ok);

safety_state_t safety_state(void);
const char *safety_state_str(safety_state_t state);

safety_arm_result_t safety_last_arm_result(void);
const char *safety_arm_result_str(safety_arm_result_t result);

bf_fault_mask_t safety_faults(void);
bool safety_has_fault(bf_fault_bit_t bit);

/* Raise a fault. Latched faults stay until safety_clear_faults(); non-latching faults
 * clear with safety_lower_fault(). */
void safety_raise_fault(bf_fault_bit_t bit, const char *detail);
void safety_lower_fault(bf_fault_bit_t bit);

/* Operator clears faults. Only succeeds when no latching condition remains. */
esp_err_t safety_clear_faults(void);

/* The session is the wireless/console control session; without it nothing can arm. */
void safety_set_session(bool established);
bool safety_session_established(void);

/* Whether the power path is currently considered valid (voltages in range, ADC sane).
 * Set by the orchestrator after evaluating sensor readings. */
void safety_set_power_ok(bool ok);

safety_arm_result_t safety_request_arm(void);
esp_err_t safety_disarm(void);
void safety_emergency_stop(void);
bool safety_estop_latched(void);

/* Advance the state machine, drive the safe-angle return and refresh annunciation. */
esp_err_t safety_tick(float dt_s);

#ifdef __cplusplus
}
#endif
