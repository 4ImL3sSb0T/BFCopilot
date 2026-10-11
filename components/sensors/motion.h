#pragma once

#include "sensors.h"

void motion_init(void);
esp_err_t motion_sample(void);
void motion_get(sensors_reading_t *out);
