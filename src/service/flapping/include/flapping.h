#pragma once

#include <stdbool.h>

typedef struct {
    float frequency_hz;
    float amplitude_deg[2];
    float center_deg[2];
    float phase_offset_deg;
    float differential;
    unsigned mode;
} flapping_command_t;

typedef struct {
    float min_deg[2];
    float max_deg[2];
    float speed_dps[2];
    float acceleration_dps2[2];
    float max_frequency_hz;
    float ramp_s;
} flapping_limits_t;

typedef struct {
    flapping_command_t current;
    flapping_command_t target;
    float phase_rad;
    bool initialized;
} flapping_t;

bool flapping_validate(const flapping_command_t *command, const flapping_limits_t *limits);
bool flapping_command(flapping_t *state, const flapping_command_t *command,
                      const flapping_limits_t *limits);
bool flapping_step(flapping_t *state, const flapping_limits_t *limits, float dt_s,
                   float output_deg[2]);
void flapping_reset(flapping_t *state);
