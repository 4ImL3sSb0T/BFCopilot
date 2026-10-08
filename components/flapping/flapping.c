#include "flapping.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define BF_PI 3.14159265358979323846f

static float amplitude(const flapping_command_t *command, unsigned side)
{
    return command->amplitude_deg[side] *
           (1.0f + (side == 0 ? command->differential : -command->differential));
}

bool flapping_validate(const flapping_command_t *command, const flapping_limits_t *limits)
{
    if (!command || !limits || command->mode > 2 ||
        !isfinite(command->frequency_hz) || command->frequency_hz < 0 ||
        !isfinite(limits->max_frequency_hz) || limits->max_frequency_hz <= 0 ||
        command->frequency_hz > limits->max_frequency_hz ||
        !isfinite(command->phase_offset_deg) || fabsf(command->phase_offset_deg) > 180 ||
        !isfinite(command->differential) || fabsf(command->differential) > 1 ||
        !isfinite(limits->ramp_s) || limits->ramp_s <= 0) {
        return false;
    }
    if (command->mode != 2 && (command->frequency_hz != 0 ||
        command->phase_offset_deg != 0 || command->differential != 0)) {
        return false;
    }
    for (unsigned side = 0; side < 2; side++) {
        float center = command->center_deg[side];
        float peak = amplitude(command, side);
        float omega = 2 * BF_PI * command->frequency_hz;
        if (!isfinite(center) || !isfinite(command->amplitude_deg[side]) ||
            command->amplitude_deg[side] < 0 ||
            !isfinite(limits->min_deg[side]) || !isfinite(limits->max_deg[side]) ||
            limits->min_deg[side] >= limits->max_deg[side] ||
            !isfinite(limits->speed_dps[side]) || limits->speed_dps[side] <= 0 ||
            !isfinite(limits->acceleration_dps2[side]) || limits->acceleration_dps2[side] <= 0 ||
            center - peak < limits->min_deg[side] || center + peak > limits->max_deg[side] ||
            peak * omega > limits->speed_dps[side] ||
            peak * omega * omega > limits->acceleration_dps2[side] ||
            (command->mode != 2 && peak != 0)) {
            return false;
        }
    }
    return true;
}

bool flapping_command(flapping_t *state, const flapping_command_t *command,
                      const flapping_limits_t *limits)
{
    if (!state || !flapping_validate(command, limits)) {
        return false;
    }
    state->target = *command;
    if (!state->initialized) {
        state->current = *command;
        state->current.amplitude_deg[0] = 0;
        state->current.amplitude_deg[1] = 0;
        state->initialized = true;
    }
    return true;
}

bool flapping_step(flapping_t *state, const flapping_limits_t *limits, float dt_s,
                   float output_deg[2])
{
    if (!state || !output_deg || !state->initialized || !isfinite(dt_s) || dt_s <= 0 ||
        dt_s > 0.02f || !flapping_validate(&state->target, limits)) {
        return false;
    }
    float blend = fminf(dt_s / limits->ramp_s, 1.0f);
    state->current.frequency_hz += blend * (state->target.frequency_hz - state->current.frequency_hz);
    state->current.phase_offset_deg += blend *
        (state->target.phase_offset_deg - state->current.phase_offset_deg);
    for (unsigned side = 0; side < 2; side++) {
        float desired = amplitude(&state->target, side);
        state->current.amplitude_deg[side] += blend *
            (desired - state->current.amplitude_deg[side]);
        state->current.center_deg[side] += blend *
            (state->target.center_deg[side] - state->current.center_deg[side]);
    }
    state->phase_rad = fmodf(state->phase_rad + 2 * BF_PI * state->current.frequency_hz * dt_s,
                            2 * BF_PI);
    for (unsigned side = 0; side < 2; side++) {
        float phase = state->phase_rad + (side == 1 ?
            state->current.phase_offset_deg * BF_PI / 180.0f : 0);
        output_deg[side] = state->current.center_deg[side] +
            state->current.amplitude_deg[side] * sinf(phase);
        output_deg[side] = fminf(limits->max_deg[side], fmaxf(limits->min_deg[side], output_deg[side]));
    }
    return true;
}

void flapping_reset(flapping_t *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}
