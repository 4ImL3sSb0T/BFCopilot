#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal bench console. This is a stand-in for the eventual host tool: it exposes
 * status, parameters, servo jogging, arming and fault clearing over the programming
 * port so Phase 1 can be exercised without the wireless link.
 */
esp_err_t bf_console_start(void);

#ifdef __cplusplus
}
#endif
