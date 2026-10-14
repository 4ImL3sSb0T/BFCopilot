#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_buzzer_init(void);

/* Set a steady tone; 0 Hz silences the buzzer. Returns immediately. */
esp_err_t board_buzzer_tone(uint32_t freq_hz);

/* Non-blocking beep sequence: `count` pulses of on_ms at freq_hz separated by off_ms.
 * count == 0 stops any sequence in progress. */
esp_err_t board_buzzer_beep(uint32_t freq_hz, uint32_t on_ms, uint32_t off_ms, uint32_t count);

#ifdef __cplusplus
}
#endif
