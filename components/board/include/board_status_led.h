#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status annunciation. Patterns map to safety states so the operator can see, without
 * any tooling, whether the vehicle is locked, armed or faulted. */
typedef enum {
    BOARD_LED_OFF = 0,
    BOARD_LED_BOOT,
    BOARD_LED_LOCKED,
    BOARD_LED_READY,
    BOARD_LED_ARMED,
    BOARD_LED_CALIB,
    BOARD_LED_FAULT,
    BOARD_LED_COUNT
} board_led_pattern_t;

esp_err_t board_led_init(void);
void board_led_set(board_led_pattern_t pattern);
board_led_pattern_t board_led_get(void);

/* Direct RGB write, mainly for bring-up. Values are 0..255. */
esp_err_t board_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
