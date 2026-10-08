#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Battery and servo-current sensing.
 *
 * The conversion constants (divider ratio, shunt value, INA199 gain and zero offset)
 * live in the parameter store and are unmeasured placeholders until bench calibration
 * sets sensors.calibrated. When they are not marked calibrated the readings are still
 * produced but flagged invalid so nothing downstream treats them as trustworthy.
 */

typedef struct {
    int32_t vbat_mv;     /* battery voltage in millivolts */
    int32_t iservo_ma;   /* servo supply current in milliamps */
    bool vbat_valid;
    bool iservo_valid;
    bool saturated;      /* either channel pinned near full scale */
} sensors_reading_t;

esp_err_t sensors_init(void);

/* Acquire one sample from both channels and fold it into the filter. */
esp_err_t sensors_sample(void);

void sensors_get_latest(sensors_reading_t *out);

bool sensors_calibration_valid(void);

/* Re-read conversion constants from the parameter store. */
esp_err_t sensors_reload_params(void);

#ifdef __cplusplus
}
#endif
