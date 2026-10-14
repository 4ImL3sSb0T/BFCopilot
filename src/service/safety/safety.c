#include "safety.h"

#include "board_buzzer.h"
#include "board_status_led.h"
#include "servo.h"
#include "parameter.h"
#include "telemetry.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "safety";

#define SAFE_ANGLE_TOL_DEG 1.0f

static safety_state_t s_state = SAFETY_BOOT_LOCKED;
static safety_arm_result_t s_last_arm = SAFETY_ARM_OK;
static bf_fault_mask_t s_faults;  /* latched, reported */
static bf_fault_mask_t s_active;  /* conditions currently true */
static bool s_session;
static bool s_power_ok;
static bool s_estop;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static const char *const s_state_names[] = {
    [SAFETY_BOOT_LOCKED] = "BOOT_LOCKED",
    [SAFETY_READY_LOCKED] = "READY_LOCKED",
    [SAFETY_ARMED] = "ARMED",
    [SAFETY_SAFE_RETURN_LOCKED] = "SAFE_RETURN_LOCKED",
    [SAFETY_LOCKED] = "LOCKED",
    [SAFETY_FAULT_LATCHED] = "FAULT_LATCHED",
};

/* Faults that, in ARMED, request a controlled return instead of an immediate latch. */
static bool is_safe_return_fault(bf_fault_bit_t bit)
{
    return bit == BF_FAULT_ESTOP || bit == BF_FAULT_LINK_LOST;
}

static void annunciate(safety_state_t st)
{
    switch (st) {
    case SAFETY_BOOT_LOCKED:      board_led_set(BOARD_LED_BOOT);  break;
    case SAFETY_READY_LOCKED:     board_led_set(BOARD_LED_READY); break;
    case SAFETY_ARMED:            board_led_set(BOARD_LED_ARMED); break;
    case SAFETY_SAFE_RETURN_LOCKED:
    case SAFETY_LOCKED:           board_led_set(BOARD_LED_LOCKED); break;
    case SAFETY_FAULT_LATCHED:    board_led_set(BOARD_LED_FAULT); break;
    default:                      board_led_set(BOARD_LED_FAULT); break;
    }
}

static void set_state(safety_state_t next)
{
    if (next == s_state) {
        return;
    }
    ESP_LOGI(TAG, "%s -> %s", safety_state_str(s_state), safety_state_str(next));
    s_state = next;
    annunciate(next);
    if (next == SAFETY_FAULT_LATCHED) {
        board_buzzer_beep(2500, 120, 80, 3);
    } else if (next == SAFETY_ARMED) {
        board_buzzer_beep(2000, 60, 0, 1);
    }
}

esp_err_t safety_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_state = SAFETY_BOOT_LOCKED;
    s_faults = BF_FAULT_MASK_NONE;
    s_active = BF_FAULT_MASK_NONE;
    s_session = false;
    s_power_ok = false;
    s_estop = false;
    servo_force_output_off();
    annunciate(s_state);
    return ESP_OK;
}

const char *safety_state_str(safety_state_t state)
{
    if ((int)state < 0 || state > SAFETY_FAULT_LATCHED) {
        return "?";
    }
    return s_state_names[state];
}

safety_state_t safety_state(void)
{
    return s_state;
}

safety_arm_result_t safety_last_arm_result(void)
{
    return s_last_arm;
}

const char *safety_arm_result_str(safety_arm_result_t result)
{
    switch (result) {
    case SAFETY_ARM_OK:                 return "ok";
    case SAFETY_ARM_ERR_STATE:          return "wrong state";
    case SAFETY_ARM_ERR_NO_SESSION:     return "no session";
    case SAFETY_ARM_ERR_FAULTS:         return "faults present";
    case SAFETY_ARM_ERR_NOT_CALIBRATED: return "servo not calibrated";
    case SAFETY_ARM_ERR_POWER:          return "power path not valid";
    default:                            return "?";
    }
}

bf_fault_mask_t safety_faults(void)
{
    return s_faults;
}

bool safety_has_fault(bf_fault_bit_t bit)
{
    return bit < BF_FAULT_COUNT && (s_faults & BF_FAULT_BIT(bit));
}

void safety_raise_fault(bf_fault_bit_t bit, const char *detail)
{
    if ((unsigned)bit >= BF_FAULT_COUNT) {
        return;
    }
    bool newly = !(s_faults & BF_FAULT_BIT(bit));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_faults |= BF_FAULT_BIT(bit);
    s_active |= BF_FAULT_BIT(bit);
    xSemaphoreGive(s_lock);

    if (newly) {
        telemetry_log_event(bit, detail != NULL ? detail : bf_fault_action(bit));
    }

    if (s_state == SAFETY_ARMED || s_state == SAFETY_SAFE_RETURN_LOCKED || servo_is_enabled()) {
        if (is_safe_return_fault(bit)) {
            set_state(SAFETY_SAFE_RETURN_LOCKED);
        } else {
            servo_force_output_off();
            set_state(SAFETY_FAULT_LATCHED);
        }
    } else if (s_state == SAFETY_BOOT_LOCKED && bit != BF_FAULT_ESTOP &&
               bit != BF_FAULT_LINK_LOST && bit != BF_FAULT_RESET_ABNORMAL) {
        /* A self-check-class fault during boot latches. */
        set_state(SAFETY_FAULT_LATCHED);
    }
}

void safety_lower_fault(bf_fault_bit_t bit)
{
    if ((unsigned)bit >= BF_FAULT_COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active &= ~BF_FAULT_BIT(bit);
    if (!bf_fault_is_latching(bit)) {
        s_faults &= ~BF_FAULT_BIT(bit);
    }
    xSemaphoreGive(s_lock);
}

esp_err_t safety_clear_faults(void)
{
    if (s_state == SAFETY_ARMED || s_state == SAFETY_SAFE_RETURN_LOCKED) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_estop = false;
    s_active &= ~BF_FAULT_BIT(BF_FAULT_ESTOP);
    /* Keep only conditions that are still actually present. */
    bf_fault_mask_t remaining = s_faults & s_active;
    s_faults = remaining;
    xSemaphoreGive(s_lock);

    if (remaining != BF_FAULT_MASK_NONE) {
        ESP_LOGW(TAG, "cannot clear: faults still active");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == SAFETY_FAULT_LATCHED || s_state == SAFETY_LOCKED ||
        s_state == SAFETY_SAFE_RETURN_LOCKED) {
        servo_force_output_off();
        set_state(s_session ? SAFETY_READY_LOCKED : SAFETY_LOCKED);
    }
    return ESP_OK;
}

void safety_boot_complete(bool self_check_ok)
{
    if (s_state != SAFETY_BOOT_LOCKED) {
        return;
    }
    set_state(self_check_ok ? SAFETY_READY_LOCKED : SAFETY_FAULT_LATCHED);
}

void safety_set_session(bool established)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_session = established;
    xSemaphoreGive(s_lock);

    if (established) {
        safety_lower_fault(BF_FAULT_LINK_LOST);
        if (s_state == SAFETY_LOCKED && !s_faults) {
            set_state(SAFETY_READY_LOCKED);
        }
    }

    if (!established && s_state == SAFETY_ARMED) {
        /* Losing the session is a link-loss class event. */
        safety_raise_fault(BF_FAULT_LINK_LOST, "session closed");
    }
}

bool safety_session_established(void)
{
    return s_session;
}

void safety_set_power_ok(bool ok)
{
    s_power_ok = ok;
}

safety_arm_result_t safety_request_arm(void)
{
    if (s_state != SAFETY_READY_LOCKED) {
        s_last_arm = SAFETY_ARM_ERR_STATE;
    } else if (!s_session) {
        s_last_arm = SAFETY_ARM_ERR_NO_SESSION;
    } else if (s_faults != BF_FAULT_MASK_NONE) {
        s_last_arm = SAFETY_ARM_ERR_FAULTS;
    } else if (!servo_calibration_valid(SERVO_LEFT) || !servo_calibration_valid(SERVO_RIGHT)) {
        s_last_arm = SAFETY_ARM_ERR_NOT_CALIBRATED;
    } else if (!s_power_ok || !parameter_is_valid() || !parameter_get()->safety.calibrated ||
               !parameter_get()->sensors.calibrated) {
        s_last_arm = SAFETY_ARM_ERR_POWER;
    } else {
        servo_return_to_safe();
        if (servo_enable(true) == ESP_OK) {
            s_last_arm = SAFETY_ARM_OK;
            set_state(SAFETY_ARMED);
        } else {
            s_last_arm = SAFETY_ARM_ERR_NOT_CALIBRATED;
        }
    }
    return s_last_arm;
}

esp_err_t safety_disarm(void)
{
    if (s_state != SAFETY_ARMED && s_state != SAFETY_SAFE_RETURN_LOCKED) {
        return ESP_ERR_INVALID_STATE;
    }
    servo_return_to_safe();
    set_state(SAFETY_SAFE_RETURN_LOCKED);
    return ESP_OK;
}

void safety_emergency_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_estop = true;
    xSemaphoreGive(s_lock);
    safety_raise_fault(BF_FAULT_ESTOP, "estop");
}

bool safety_estop_latched(void)
{
    return s_estop;
}

static bool servos_at_safe(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        float out = 0.0f;
        float safe = 0.0f;
        if (servo_get_output_deg((servo_idx_t)i, &out) != ESP_OK ||
            servo_get_safe_deg((servo_idx_t)i, &safe) != ESP_OK) {
            return false;
        }
        if (fabsf(out - safe) > SAFE_ANGLE_TOL_DEG) {
            return false;
        }
    }
    return true;
}

esp_err_t safety_tick(float dt_s)
{
    switch (s_state) {
    case SAFETY_ARMED:
        if (s_estop) {
            /* estop latch already raised the fault, but keep it robust. */
            set_state(SAFETY_SAFE_RETURN_LOCKED);
        }
        break;

    case SAFETY_SAFE_RETURN_LOCKED:
        servo_enable(true);
        servo_return_to_safe();
        if (servos_at_safe()) {
            set_state(SAFETY_LOCKED);
        }
        break;

    case SAFETY_FAULT_LATCHED:
        servo_force_output_off();
        break;

    case SAFETY_BOOT_LOCKED:
    case SAFETY_READY_LOCKED:
    case SAFETY_LOCKED:
    default:
        break;
    }

    (void)dt_s;
    return ESP_OK;
}
