#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "flapping.h"

/*
 * Bench runner for the flapping service (test/bench only).
 *
 * It exists so the trajectory generator can be exercised on real servos without the flight
 * stack around it: no session, no lease, no arming state - the console command takes the
 * place of the CONTROL frame. Everything downstream of the command is the flight path:
 * flapping_command()/flapping_step() drive servo_set_target_deg() and servo_update() at the
 * same period the flight controller ticks at, against the same parameters.
 *
 * It can only move a servo whose calibration is valid. servo_enable() refuses unless both
 * sides are calibrated, and servo_update() holds an uncalibrated side low, so a fresh board
 * must have its two-point calibration filled in before this does anything visible.
 */

typedef struct {
    bool running;
    bool timed;                 /* a duration was given and is still counting down */
    float remaining_s;
    uint32_t steps;             /* ticks the trajectory advanced */
    uint32_t overruns;          /* ticks whose measured dt was outside what flapping_step() accepts */
    uint32_t refused;           /* ticks the trajectory step or the servo push rejected */
    float frequency_hz;         /* ramped values that were actually output, not the target */
    float amplitude_deg[2];
    float center_deg[2];
    float phase_deg;
    float output_deg[2];
    flapping_command_t target;
} bf_flap_status_t;

esp_err_t bf_flap_init(void);

/* Start, or retarget a running trajectory. `seconds` <= 0 runs until bf_flap_stop().
 * Returns ESP_ERR_INVALID_ARG when flapping_validate() rejects the command, and
 * ESP_ERR_INVALID_STATE when a servo has no valid calibration. */
esp_err_t bf_flap_apply(const flapping_command_t *command, float seconds);

/* Stop the trajectory and hold both generators low. Safe to call when not running. */
void bf_flap_stop(void);

bool bf_flap_running(void);
void bf_flap_get_status(bf_flap_status_t *out);
