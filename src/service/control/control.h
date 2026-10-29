#pragma once

#include "bsp/servo/bsp_servo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lib/tools/vec_math.h"

typedef struct {
    vec2f frq;
    vec2f max_angle;
    f32 phase_diff;
} control_command_t;

esp_err_t control_service_init(f32 servo_max_speed, f32 servo_max_acc);

esp_err_t control_service_excute(control_command_t command);

esp_err_t control_service_stop();