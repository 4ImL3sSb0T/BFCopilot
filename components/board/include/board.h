#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Board capabilities. This describes what the current PCB can actually do, so callers
 * can branch on hardware reality instead of pretending a missing feature exists.
 */
typedef struct {
    const char *hw_name;
    const char *hw_revision;
    bool has_servo_power_switch; /* false on the current board: PWM off != power off */
    bool has_imu;
    bool has_baro;
    bool has_vbat_adc;
    bool has_current_adc;
    bool has_ws2812;
    bool has_buzzer;
    bool usb_native; /* 0 = unknown/not verified */
} board_caps_t;

typedef enum {
    BOARD_SERVO_POWER_UNSUPPORTED = 0, /* no switch fitted */
    BOARD_SERVO_POWER_OFF,
    BOARD_SERVO_POWER_ON,
} board_servo_power_t;

const board_caps_t *board_caps(void);

/* Common early init: logging, reset-reason capture, buzzer and status LED. */
esp_err_t board_init(void);

/* Servo power switch hooks. On this board both return ESP_ERR_NOT_SUPPORTED and change
 * nothing; they exist so the next hardware revision can implement them without
 * touching callers. Stopping PWM must never be reported as cutting power. */
esp_err_t board_servo_power_enable(void);
esp_err_t board_servo_power_disable(void);
board_servo_power_t board_servo_power_state(void);

/* Human-readable form of esp_reset_reason() for the current boot. */
const char *board_reset_reason_str(void);

#ifdef __cplusplus
}
#endif
