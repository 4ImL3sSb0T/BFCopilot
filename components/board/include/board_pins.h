#pragma once

/*
 * Single source of truth for the current board's pin map.
 *
 * These values come from the hand-off netlist and the user's confirmation that the
 * current PCB keeps the original pins. They are NOT proof of every electrical
 * parameter: divider ratios, shunt value, INA199 gain and servo supply details must
 * still be measured (see docs/hardware-baseline.md). No business code may hardcode a
 * pin number; include this header instead.
 */

/* Actuators */
#define BOARD_PIN_SERVO_LEFT_PWM    18
#define BOARD_PIN_SERVO_RIGHT_PWM   19

/* IMU: LSM6DS3TR-C on SPI, checked against the live schematic 2026-10-11. */
#define BOARD_PIN_IMU_SCK           7
#define BOARD_PIN_IMU_MOSI          15
#define BOARD_PIN_IMU_MISO          14
#define BOARD_PIN_IMU_CS            6

/* Barometer: SPA06-003 on I2C, default address 0x77 */
#define BOARD_PIN_BARO_SCL          4
#define BOARD_PIN_BARO_SDA          5
#define BOARD_BARO_I2C_ADDR         0x77

/* Analog sense. Divider ratio / shunt value are unmeasured; see parameter defaults. */
#define BOARD_PIN_VBAT_ADC          0
#define BOARD_PIN_ISERVO_ADC        1

/* Annunciation */
#define BOARD_PIN_BUZZER            3
#define BOARD_PIN_WS2812            23

/* Native USB data lines (device capability depends on the module, verify on bench) */
#define BOARD_PIN_USB_DM            12
#define BOARD_PIN_USB_DP            13

/* Board identity. The physical revision has not been photographed/recorded yet. */
#define BOARD_HW_NAME               "butterfly-current"
#define BOARD_HW_REVISION           "unverified"
