#pragma once

#include "bsp/servo/bsp_servo.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lib/tools/vec_math.h"

typedef enum {
    CONTROL_HOLD = 0,
    CONTROL_MANUAL = 1,
    CONTROL_FLAP = 2,
} control_mode_t;

typedef struct {
    control_mode_t mode;
    f32 frequency_hz;
    vec2f amplitude_deg;
    vec2f center_deg;
    f32 phase_diff_deg;   /* 右侧相对左侧 */
    f32 differential;     /* -1..1，左×(1+d)，右×(1-d) */
} control_command_t;

esp_err_t control_service_init(f32 servo_max_speed, f32 servo_max_acc);

esp_err_t control_service_excute(control_command_t command);

esp_err_t control_service_start();

esp_err_t control_service_stop();