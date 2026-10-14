#pragma once

#include <stdint.h>

typedef struct {
    uint8_t calibrated;
    uint16_t vbat_div_num;      /* battery divider ratio numerator */
    uint16_t vbat_div_den;      /* battery divider ratio denominator */
    uint16_t iservo_shunt_mohm; /* shunt resistance in milliohms */
    uint16_t ina199_gain;       /* INA199 gain, V/V */
    int32_t iservo_zero_mv;     /* INA199 output at zero current, must be measured */
} bf_sensors_cfg_t;
