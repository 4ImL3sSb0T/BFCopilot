#include "servo.h"

#include "board_pins.h"
#include "parameter.h"
#include "servo_math.h"

#include "driver/mcpwm_prelude.h"
#include "esp_log.h"

static const char *TAG = "servo";

#define MCPWM_GROUP_ID       0
#define MCPWM_RESOLUTION_HZ  1000000u /* 1 tick = 1 us */
#define MCPWM_PERIOD_TICKS   3003u    /* 1e6 / 3003 ~= 333.0 Hz */

typedef struct {
    int gpio;
    bf_servo_id_t pid;
    mcpwm_cmpr_handle_t cmpr;
    mcpwm_gen_handle_t gen;
    float target_deg;
    float output_deg;
    float velocity_dps;
    bool pulsing; /* generator is currently allowed to drive */
} servo_ctx_t;

static servo_ctx_t s_servo[SERVO_COUNT] = {
    { .gpio = BOARD_PIN_SERVO_LEFT_PWM,  .pid = BF_SERVO_LEFT },
    { .gpio = BOARD_PIN_SERVO_RIGHT_PWM, .pid = BF_SERVO_RIGHT },
};

static mcpwm_timer_handle_t s_timer;
static mcpwm_oper_handle_t s_oper;

/* Bench path state (see servo.h). MCPWM has no compare-value readback, so this records
 * what the bench path wrote rather than measuring the pin. */
static float s_debug_pulse_us[SERVO_COUNT];
static bool s_debug_active[SERVO_COUNT];
static bool s_init_done;
static bool s_enabled;

static const bf_servo_cal_t *cal_of(servo_idx_t s)
{
    return &parameter_get()->servo[s_servo[s].pid];
}

bool servo_calibration_valid(servo_idx_t servo)
{
    if ((unsigned)servo >= SERVO_COUNT) {
        return false;
    }
    const bf_servo_cal_t *cal = cal_of(servo);
    return cal->calibrated != 0 &&
           cal->angle_max_deg > cal->angle_min_deg &&
           cal->angle_min_us > 0 && cal->angle_max_us > 0 &&
           cal->max_speed_dps > 0.0f;
}

/* Map a commanded angle to a pulse width using the two-point calibration. */
static esp_err_t angle_to_pulse_us(const bf_servo_cal_t *cal, float deg, float *out_us)
{
    if (!cal->calibrated) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!servo_map_angle_to_pulse_us(deg, cal->direction, cal->angle_min_deg,
                                     cal->angle_max_deg, cal->angle_min_us,
                                     cal->angle_max_us, out_us)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (*out_us < SERVO_PULSE_SANITY_MIN_US) *out_us = SERVO_PULSE_SANITY_MIN_US;
    if (*out_us > SERVO_PULSE_SANITY_MAX_US) *out_us = SERVO_PULSE_SANITY_MAX_US;
    return ESP_OK;
}

static esp_err_t drive_off(servo_idx_t s)
{
    esp_err_t err = mcpwm_generator_set_force_level(s_servo[s].gen, 0, true);
    if (err == ESP_OK) {
        s_servo[s].pulsing = false;
    }
    return err;
}

static esp_err_t drive_on(servo_idx_t s)
{
    esp_err_t err = mcpwm_generator_set_force_level(s_servo[s].gen, -1, false);
    if (err == ESP_OK) {
        s_servo[s].pulsing = true;
    }
    return err;
}

esp_err_t servo_init(void)
{
    if (s_init_done) {
        return ESP_OK;
    }

    mcpwm_timer_config_t timer_cfg = {
        .group_id = MCPWM_GROUP_ID,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = MCPWM_RESOLUTION_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = MCPWM_PERIOD_TICKS,
    };
    esp_err_t err = mcpwm_new_timer(&timer_cfg, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm_new_timer: %s", esp_err_to_name(err));
        return err;
    }

    mcpwm_operator_config_t oper_cfg = { .group_id = MCPWM_GROUP_ID };
    err = mcpwm_new_operator(&oper_cfg, &s_oper);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mcpwm_new_operator: %s", esp_err_to_name(err));
        return err;
    }
    err = mcpwm_operator_connect_timer(s_oper, s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect timer: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        mcpwm_comparator_config_t cmpr_cfg = { .flags.update_cmp_on_tez = true };
        err = mcpwm_new_comparator(s_oper, &cmpr_cfg, &s_servo[i].cmpr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "new comparator %d: %s", i, esp_err_to_name(err));
            return err;
        }

        mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = s_servo[i].gpio };
        err = mcpwm_new_generator(s_oper, &gen_cfg, &s_servo[i].gen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "new generator %d: %s", i, esp_err_to_name(err));
            return err;
        }

        /* Rise on timer empty, fall on compare match: pulse width == compare value. */
        err = mcpwm_generator_set_action_on_timer_event(
            s_servo[i].gen,
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY,
                                         MCPWM_GEN_ACTION_HIGH));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "timer action %d: %s", i, esp_err_to_name(err));
            return err;
        }
        err = mcpwm_generator_set_action_on_compare_event(
            s_servo[i].gen,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_servo[i].cmpr,
                                           MCPWM_GEN_ACTION_LOW));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "compare action %d: %s", i, esp_err_to_name(err));
            return err;
        }

        /* Hold low until explicitly enabled: no pulse at boot, ever. */
        ESP_ERROR_CHECK(drive_off((servo_idx_t)i));
        s_servo[i].target_deg = 0.0f;
        s_servo[i].output_deg = 0.0f;
    }

    err = mcpwm_timer_enable(s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer enable: %s", esp_err_to_name(err));
        return err;
    }
    err = mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer start: %s", esp_err_to_name(err));
        return err;
    }
    s_init_done = true;
    s_enabled = false;

    ESP_LOGI(TAG, "MCPWM up at %u Hz, both generators held low",
             MCPWM_RESOLUTION_HZ / MCPWM_PERIOD_TICKS);
    return ESP_OK;
}

bool servo_is_enabled(void)
{
    return s_enabled;
}

esp_err_t servo_enable(bool enable)
{
    if (!s_init_done) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable && (!servo_calibration_valid(SERVO_LEFT) || !servo_calibration_valid(SERVO_RIGHT))) {
        servo_force_output_off();
        return ESP_ERR_INVALID_STATE;
    }
    s_enabled = enable;

    for (int i = 0; i < SERVO_COUNT; i++) {
        const bf_servo_cal_t *cal = cal_of((servo_idx_t)i);
        if (enable && servo_calibration_valid((servo_idx_t)i)) {
            float pulse;
            if (angle_to_pulse_us(cal, s_servo[i].output_deg, &pulse) != ESP_OK) {
                continue;
            }
            esp_err_t err = mcpwm_comparator_set_compare_value(s_servo[i].cmpr, (uint32_t)(pulse + 0.5f));
            if (err == ESP_OK) {
                err = drive_on((servo_idx_t)i);
            }
            if (err != ESP_OK) {
                servo_force_output_off();
                return err;
            }
        } else {
            drive_off((servo_idx_t)i);
        }
    }
    return ESP_OK;
}

esp_err_t servo_set_target_deg(servo_idx_t servo, float deg)
{
    if ((unsigned)servo >= SERVO_COUNT || !isfinite(deg)) {
        return ESP_ERR_INVALID_ARG;
    }
    const bf_servo_cal_t *cal = cal_of(servo);
    float clamped = deg;
    if (servo_calibration_valid(servo)) {
        if (clamped < cal->angle_min_deg) clamped = cal->angle_min_deg;
        if (clamped > cal->angle_max_deg) clamped = cal->angle_max_deg;
    }
    s_servo[servo].target_deg = clamped;
    return ESP_OK;
}

esp_err_t servo_get_target_deg(servo_idx_t servo, float *out)
{
    if ((unsigned)servo >= SERVO_COUNT || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_servo[servo].target_deg;
    return ESP_OK;
}

esp_err_t servo_get_output_deg(servo_idx_t servo, float *out)
{
    if ((unsigned)servo >= SERVO_COUNT || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_servo[servo].output_deg;
    return ESP_OK;
}

esp_err_t servo_get_pulse_us(servo_idx_t servo, float *out)
{
    if ((unsigned)servo >= SERVO_COUNT || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    float pulse;
    esp_err_t err = angle_to_pulse_us(cal_of(servo), s_servo[servo].output_deg, &pulse);
    if (err == ESP_OK) {
        *out = pulse;
    }
    return err;
}

esp_err_t servo_get_safe_deg(servo_idx_t servo, float *out)
{
    if ((unsigned)servo >= SERVO_COUNT || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = cal_of(servo)->safe_deg;
    return ESP_OK;
}

esp_err_t servo_return_to_safe(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (!servo_calibration_valid((servo_idx_t)i)) {
            continue;
        }
        servo_set_target_deg((servo_idx_t)i, cal_of((servo_idx_t)i)->safe_deg);
    }
    return ESP_OK;
}

esp_err_t servo_reload_params(void)
{
    servo_force_output_off();
    for (int i = 0; i < SERVO_COUNT; i++) {
        s_servo[i].target_deg = cal_of((servo_idx_t)i)->safe_deg;
        s_servo[i].output_deg = s_servo[i].target_deg;
        s_servo[i].velocity_dps = 0;
        if (!servo_calibration_valid((servo_idx_t)i)) {
            ESP_LOGW(TAG, "servo %d calibration invalid", i);
            drive_off((servo_idx_t)i);
        }
    }
    return ESP_OK;
}

void servo_force_output_off(void)
{
    if (!s_init_done) {
        return;
    }
    for (int i = 0; i < SERVO_COUNT; i++) {
        drive_off((servo_idx_t)i);
        s_debug_active[i] = false;
    }
    s_enabled = false;
}

esp_err_t servo_update(float dt_s)
{
    if (!s_init_done || !isfinite(dt_s) || dt_s <= 0.0f || dt_s > 0.02f) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < SERVO_COUNT; i++) {
        servo_idx_t idx = (servo_idx_t)i;
        if (!servo_calibration_valid(idx)) {
            drive_off(idx);
            continue;
        }

        const bf_servo_cal_t *cal = cal_of(idx);
        if (!s_enabled) {
            s_servo[i].velocity_dps = 0;
            continue;
        }
        s_servo[i].output_deg = servo_slew(s_servo[i].output_deg, s_servo[i].target_deg,
            &s_servo[i].velocity_dps, cal->max_speed_dps, cal->max_acceleration_dps2, dt_s);
        s_servo[i].output_deg = fminf(cal->angle_max_deg, fmaxf(cal->angle_min_deg, s_servo[i].output_deg));

        float pulse;
        if (angle_to_pulse_us(cal, s_servo[i].output_deg, &pulse) != ESP_OK) {
            drive_off(idx);
            continue;
        }
        esp_err_t err = mcpwm_comparator_set_compare_value(s_servo[i].cmpr, (uint32_t)(pulse + 0.5f));
        if (err != ESP_OK) {
            servo_force_output_off();
            return err;
        }
        if (s_enabled && !s_servo[i].pulsing) {
            drive_on(idx);
        }
    }
    return ESP_OK;
}

/*
 * Bench-only raw pulse control. See servo.h: the point is to move a servo on a board that
 * has no calibration yet, so this deliberately skips the calibrated mapping.
 */

esp_err_t servo_debug_set_pulse_us(servo_idx_t s, float pulse_us)
{
    if ((unsigned)s >= SERVO_COUNT || !isfinite(pulse_us)) return ESP_ERR_INVALID_ARG;
    if (!s_init_done) return ESP_ERR_INVALID_STATE;
    if (pulse_us < SERVO_PULSE_SANITY_MIN_US) pulse_us = SERVO_PULSE_SANITY_MIN_US;
    if (pulse_us > SERVO_PULSE_SANITY_MAX_US) pulse_us = SERVO_PULSE_SANITY_MAX_US;
    esp_err_t err = mcpwm_comparator_set_compare_value(s_servo[s].cmpr,
                                                       (uint32_t)(pulse_us + 0.5f));
    if (err != ESP_OK) return err;
    s_debug_pulse_us[s] = pulse_us;
    s_debug_active[s] = true;
    if (!s_servo[s].pulsing) return drive_on(s);
    return ESP_OK;
}

esp_err_t servo_debug_get_pulse_us(servo_idx_t s, float *out_us)
{
    if ((unsigned)s >= SERVO_COUNT || !out_us) return ESP_ERR_INVALID_ARG;
    if (!s_debug_active[s]) return ESP_ERR_INVALID_STATE;
    *out_us = s_debug_pulse_us[s];
    return ESP_OK;
}

esp_err_t servo_debug_off(servo_idx_t s)
{
    if ((unsigned)s >= SERVO_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_init_done) return ESP_ERR_INVALID_STATE;
    s_debug_active[s] = false;
    return drive_off(s);
}

esp_err_t servo_debug_set_angle_deg(servo_idx_t s, float deg)
{
    if ((unsigned)s >= SERVO_COUNT || !isfinite(deg)) return ESP_ERR_INVALID_ARG;
    if (!s_init_done) return ESP_ERR_INVALID_STATE;
    float pulse = 0;
    esp_err_t err = angle_to_pulse_us(cal_of(s), deg, &pulse);
    if (err != ESP_OK) return err;
    err = servo_debug_set_pulse_us(s, pulse);
    if (err != ESP_OK) return err;
    s_servo[s].output_deg = deg;
    s_servo[s].target_deg = deg;
    return ESP_OK;
}
