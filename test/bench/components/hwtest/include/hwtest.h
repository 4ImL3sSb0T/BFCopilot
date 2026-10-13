#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the bench firmware's low-level hardware test commands (`hw ...`). Must be
 * called after bf_console_start(), which initialises the console these attach to.
 */
esp_err_t bf_hwtest_register(void);

#ifdef __cplusplus
}
#endif
