#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Raw bench access to the two sensor buses.
 *
 * These deliberately bypass the drivers' identity and configuration checks: their whole
 * purpose is to interrogate a device the driver has already rejected, so a board with an
 * unsoldered, unpowered or miswired part can still be measured. Nothing on the flight
 * path may call them - the bench firmware's `hw` commands are the only caller.
 *
 * Register and address arguments are raw: the IMU register address is the chip's own
 * (this layer adds the read bit), and the I2C address is the 7-bit one.
 */

/* Identity as documented in docs/sensors.md. Kept here so a probe and the driver cannot
 * disagree about what they are looking for. */
#define SENSORS_DEBUG_IMU_ID_REG 0x0fu
#define SENSORS_DEBUG_IMU_ID 0x6au      /* LSM6DS3TR-C WHO_AM_I */
#define SENSORS_DEBUG_BARO_ID_REG 0x0du
#define SENSORS_DEBUG_BARO_ID 0x11u     /* SPA06-003 */

/* False when the bus never came up, which makes the calls below fail rather than
 * measure anything meaningful. */
bool sensors_debug_imu_bus_ready(void);
bool sensors_debug_i2c_bus_ready(void);

/* IMU on SPI2. Usable even after the identity check failed, because the bus and device
 * handle are created before the first register read. */
esp_err_t sensors_debug_imu_read(uint8_t reg, uint8_t *data, size_t length);
esp_err_t sensors_debug_imu_write(uint8_t reg, uint8_t value);

/* Barometer bus (I2C0). The address is used exactly as given, so 0x77 and 0x76 can be
 * compared even when neither answered during init. */
esp_err_t sensors_debug_i2c_probe(uint8_t address);
esp_err_t sensors_debug_i2c_read(uint8_t address, uint8_t reg, uint8_t *data, size_t length);
esp_err_t sensors_debug_i2c_write(uint8_t address, uint8_t reg, uint8_t value);

/* Idle logic level of the diagnostic lines, or -1 when that bus is down. A line stuck at
 * 0 with a device that should idle high is the usual soldering or power symptom. */
void sensors_debug_line_levels(int *imu_miso, int *i2c_scl, int *i2c_sda);

/* One raw conversion per ADC channel plus its calibrated millivolts (-1 when no
 * calibration scheme exists). This does not feed the averaging filter the flight path
 * uses, so a bench read cannot perturb it. Any output may be NULL. */
esp_err_t sensors_debug_adc_raw(int *vbat_raw, int *iservo_raw, int *vbat_mv, int *iservo_mv);

#ifdef __cplusplus
}
#endif
