/*
 * Bench runner for the flapping service. See flap.h for what it is and what it is not.
 */
#include "flap.h"

#include <math.h>

#include "bf_runtime.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "parameter.h"
#include "servo.h"

static const char *TAG = "flap";

#define FLAP_TASK_STACK 4096
#define FLAP_TASK_PRIO  5
#define FLAP_PHASE_TO_DEG 57.2957795f

/* flapping_step() refuses a step longer than this; it matches the flight controller's
 * assumption that a tick is ~3 ms, not that the trajectory is limited to 50 Hz. */
#define FLAP_MAX_DT_S 0.02f

static flapping_t s_state;
static flapping_command_t s_command;
static float s_output[2];
static bool s_running;
static bool s_timed;
static int64_t s_deadline_us;
static int64_t s_last_us;
static uint32_t s_steps;
static uint32_t s_overruns;
static uint32_t s_refused;
static TaskHandle_t s_task;
static esp_timer_handle_t s_timer;

/* Mirrors flight.c's limits(). The parameter store owns every bound the trajectory is
 * validated against, so the bench reads the same ones instead of inventing its own. */
static flapping_limits_t limits(void)
{
    const bf_params_t *params = parameter_get();
    flapping_limits_t result = { .max_frequency_hz = params->system.max_flapping_hz,
                                 .ramp_s = params->system.ramp_s };
    for (unsigned side = 0; side < 2; side++) {
        result.min_deg[side] = params->servo[side].angle_min_deg;
        result.max_deg[side] = params->servo[side].angle_max_deg;
        result.speed_dps[side] = params->servo[side].max_speed_dps;
        result.acceleration_dps2[side] = params->servo[side].max_acceleration_dps2;
    }
    return result;
}

/* Call inside the runtime lock. */
static void stop_locked(void)
{
    s_running = false;
    s_timed = false;
    flapping_reset(&s_state);
    if (s_timer != NULL) {
        esp_timer_stop(s_timer);
    }
    servo_force_output_off();
}

static void flap_timer_cb(void *arg)
{
    (void)arg;
    BaseType_t higher = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &higher);
    portYIELD_FROM_ISR(higher);
}

static void flap_tick_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        float dt = (float)(now - s_last_us) / 1000000.0f;

        bf_runtime_lock();
        s_last_us = now;
        if (s_running) {
            if (s_timed && now >= s_deadline_us) {
                stop_locked();
                ESP_LOGI(TAG, "duration reached; output off");
            } else if (!(dt > 0.0f) || dt > FLAP_MAX_DT_S) {
                /* The tick ran late. Skipping keeps the trajectory honest - the next tick
                 * measures its own dt from now - and the count is reported rather than
                 * hidden, so a console that starves this task is visible in `flap`. */
                s_overruns++;
            } else {
                flapping_limits_t bounds = limits();
                float output[2];
                if (!flapping_step(&s_state, &bounds, dt, output)) {
                    s_refused++;
                } else {
                    for (unsigned side = 0; side < 2; side++) {
                        servo_set_target_deg((servo_idx_t)side, output[side]);
                    }
                    s_output[0] = output[0];
                    s_output[1] = output[1];
                    s_steps++;
                    if (servo_update(dt) != ESP_OK) {
                        s_refused++;
                    }
                }
            }
        }
        bf_runtime_unlock();
    }
}

esp_err_t bf_flap_init(void)
{
    if (xTaskCreate(flap_tick_task, "flap_tick", FLAP_TASK_STACK, NULL, FLAP_TASK_PRIO,
                    &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = { .callback = flap_timer_cb, .name = "flap" };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err != ESP_OK) {
        return err;
    }
    flapping_reset(&s_state);
    return ESP_OK;
}

esp_err_t bf_flap_apply(const flapping_command_t *command, float seconds)
{
    if (command == NULL || !isfinite(seconds)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!servo_calibration_valid(SERVO_LEFT) || !servo_calibration_valid(SERVO_RIGHT)) {
        return ESP_ERR_INVALID_STATE;
    }

    bf_runtime_lock();
    flapping_limits_t bounds = limits();
    bool accepted = flapping_command(&s_state, command, &bounds);
    if (accepted) {
        /* Take the pins back from the raw-pulse path, then enable: servo_enable() is what
         * lets servo_update() drive a compare value at all. */
        servo_debug_off(SERVO_LEFT);
        servo_debug_off(SERVO_RIGHT);
        esp_err_t err = servo_enable(true);
        if (err != ESP_OK) {
            stop_locked();
            bf_runtime_unlock();
            return err;
        }
        s_command = *command;
        if (!s_running) {
            s_running = true;
            s_steps = 0;
            s_overruns = 0;
            s_refused = 0;
            s_last_us = esp_timer_get_time();
            esp_err_t started = esp_timer_start_periodic(
                s_timer, parameter_get()->system.control_period_us);
            if (started != ESP_OK) {
                stop_locked();
                bf_runtime_unlock();
                return started;
            }
        }
        s_timed = seconds > 0.0f;
        if (s_timed) {
            s_deadline_us = esp_timer_get_time() + (int64_t)(seconds * 1000000.0f);
        }
    }
    bf_runtime_unlock();
    return accepted ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void bf_flap_stop(void)
{
    bf_runtime_lock();
    bool was_running = s_running;
    stop_locked();
    bf_runtime_unlock();
    if (was_running) {
        ESP_LOGI(TAG, "stopped; both generators held low");
    }
}

bool bf_flap_running(void)
{
    bf_runtime_lock();
    bool running = s_running;
    bf_runtime_unlock();
    return running;
}

void bf_flap_get_status(bf_flap_status_t *out)
{
    if (out == NULL) {
        return;
    }
    bf_runtime_lock();
    out->running = s_running;
    out->timed = s_timed;
    out->remaining_s = s_timed ? (float)(s_deadline_us - esp_timer_get_time()) / 1000000.0f : 0.0f;
    out->steps = s_steps;
    out->overruns = s_overruns;
    out->refused = s_refused;
    out->frequency_hz = s_state.current.frequency_hz;
    out->amplitude_deg[0] = s_state.current.amplitude_deg[0];
    out->amplitude_deg[1] = s_state.current.amplitude_deg[1];
    out->center_deg[0] = s_state.current.center_deg[0];
    out->center_deg[1] = s_state.current.center_deg[1];
    out->phase_deg = s_state.phase_rad * FLAP_PHASE_TO_DEG;
    out->output_deg[0] = s_output[0];
    out->output_deg[1] = s_output[1];
    out->target = s_command;
    bf_runtime_unlock();
}
