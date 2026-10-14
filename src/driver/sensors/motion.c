#include "motion.h"
#include "motion_math.h"
#include "sensors_debug.h"
#include "board_pins.h"

#include <math.h>
#include <string.h>

#include "lsm6ds3tr-c_reg.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BARO_CHIP_ID 0x11
#define IMU_MAX_AGE_US 100000
#define BARO_MAX_AGE_US 250000

static const char *TAG = "motion";
static spi_device_handle_t s_imu;
static stmdev_ctx_t s_imu_ctx;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_baro;
static bool s_imu_ready;
static bool s_baro_ready;
static spa06_coefficients_t s_coefficients;
static sensors_reading_t s_sample;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_baro_raw[6];
static int64_t s_temperature_us;

/*
 * ST 的驱动只做寄存器语义，总线由这里提供。SPI 读要置地址最高位；ST 的参考例程靠手动
 * CS 横跨"发地址/收数据"两次调用，而 ESP-IDF 硬件 CS 每次事务都会释放，所以地址走
 * command phase、数据走同一事务的数据阶段，一次调用内 CS 保持有效。
 * 回调返回 0 表示成功，失败时原样回传 esp_err_t，便于调用方恢复错误码。
 */
static int32_t imu_read(void *handle, uint8_t reg, uint8_t *data, uint16_t length)
{
    (void)handle;
    spi_transaction_t transaction = {
        .cmd = (uint16_t)(reg | 0x80),
        .length = (size_t)length * 8,
        .rx_buffer = data,
    };
    return (int32_t)spi_device_polling_transmit(s_imu, &transaction);
}

static int32_t imu_write(void *handle, uint8_t reg, const uint8_t *data, uint16_t length)
{
    (void)handle;
    spi_transaction_t transaction = {
        .cmd = reg,
        .length = (size_t)length * 8,
        .tx_buffer = data,
    };
    return (int32_t)spi_device_polling_transmit(s_imu, &transaction);
}

static void imu_delay(uint32_t millisec)
{
    vTaskDelay(pdMS_TO_TICKS(millisec));
}

static esp_err_t imu_status(int32_t ret)
{
    return ret == 0 ? ESP_OK : (esp_err_t)ret;
}

static esp_err_t verify_imu_config(void)
{
    lsm6ds3tr_c_odr_xl_t xl_odr;
    lsm6ds3tr_c_odr_g_t gy_odr;
    lsm6ds3tr_c_fs_xl_t xl_full_scale;
    lsm6ds3tr_c_fs_g_t gy_full_scale;
    uint8_t block_update = 0;
    uint8_t auto_increment = 0;
    int32_t ret = lsm6ds3tr_c_xl_data_rate_get(&s_imu_ctx, &xl_odr);
    if (ret == 0) ret = lsm6ds3tr_c_gy_data_rate_get(&s_imu_ctx, &gy_odr);
    if (ret == 0) ret = lsm6ds3tr_c_xl_full_scale_get(&s_imu_ctx, &xl_full_scale);
    if (ret == 0) ret = lsm6ds3tr_c_gy_full_scale_get(&s_imu_ctx, &gy_full_scale);
    if (ret == 0) ret = lsm6ds3tr_c_block_data_update_get(&s_imu_ctx, &block_update);
    if (ret == 0) ret = lsm6ds3tr_c_auto_increment_get(&s_imu_ctx, &auto_increment);
    if (ret != 0) return imu_status(ret);
    if (xl_odr != LSM6DS3TR_C_XL_ODR_104Hz || gy_odr != LSM6DS3TR_C_GY_ODR_104Hz ||
        xl_full_scale != LSM6DS3TR_C_2g || gy_full_scale != LSM6DS3TR_C_250dps ||
        block_update != PROPERTY_ENABLE || auto_increment != PROPERTY_ENABLE) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t init_imu(void)
{
    ESP_LOGI(TAG, "IMU pins: SCK=%d MOSI=%d MISO=%d CS=%d, SPI mode=3 1MHz",
             BOARD_PIN_IMU_SCK, BOARD_PIN_IMU_MOSI, BOARD_PIN_IMU_MISO, BOARD_PIN_IMU_CS);
    spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_PIN_IMU_MOSI,
        .miso_io_num = BOARD_PIN_IMU_MISO,
        .sclk_io_num = BOARD_PIN_IMU_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_DISABLED);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t device_config = {
        .command_bits = 8,
        .clock_speed_hz = 1000000,
        .mode = 3,
        .spics_io_num = BOARD_PIN_IMU_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &device_config, &s_imu);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));

    s_imu_ctx.write_reg = imu_write;
    s_imu_ctx.read_reg = imu_read;
    s_imu_ctx.mdelay = imu_delay;

    uint8_t id = 0;
    int32_t ret = 0;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        ret = lsm6ds3tr_c_device_id_get(&s_imu_ctx, &id);
        if (ret == 0 && id == LSM6DS3TR_C_ID) break;
        ESP_LOGW(TAG, "IMU identity attempt %u: transfer=%s WHO_AM_I=0x%02x MISO=%d",
                 attempt + 1, esp_err_to_name(imu_status(ret)), id,
                 gpio_get_level(BOARD_PIN_IMU_MISO));
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ret != 0) return imu_status(ret);
    if (id != LSM6DS3TR_C_ID) {
        ESP_LOGW(TAG, "LSM6DS3TR-C WHO_AM_I=0x%02x, expected 0x%02x", id, LSM6DS3TR_C_ID);
        return ESP_ERR_NOT_FOUND;
    }
    ret = lsm6ds3tr_c_reset_set(&s_imu_ctx, PROPERTY_ENABLE);
    if (ret != 0) return imu_status(ret);
    int64_t deadline = esp_timer_get_time() + 100000;
    uint8_t reset = 1;
    while (reset) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(1));
        ret = lsm6ds3tr_c_reset_get(&s_imu_ctx, &reset);
        if (ret != 0) return imu_status(ret);
    }
    ret = lsm6ds3tr_c_block_data_update_set(&s_imu_ctx, PROPERTY_ENABLE);
    if (ret == 0) ret = lsm6ds3tr_c_auto_increment_set(&s_imu_ctx, PROPERTY_ENABLE);
    if (ret == 0) ret = lsm6ds3tr_c_xl_data_rate_set(&s_imu_ctx, LSM6DS3TR_C_XL_ODR_104Hz);
    if (ret == 0) ret = lsm6ds3tr_c_gy_data_rate_set(&s_imu_ctx, LSM6DS3TR_C_GY_ODR_104Hz);
    if (ret == 0) ret = lsm6ds3tr_c_xl_full_scale_set(&s_imu_ctx, LSM6DS3TR_C_2g);
    if (ret == 0) ret = lsm6ds3tr_c_gy_full_scale_set(&s_imu_ctx, LSM6DS3TR_C_250dps);
    if (ret != 0) return imu_status(ret);
    err = verify_imu_config();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "LSM6DS3TR-C SPI: 104 Hz, +/-2 g, +/-250 dps, chip axes (ST driver 2.2.3)");
    return ESP_OK;
}

static esp_err_t baro_read(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(s_baro, &reg, 1, data, length, 10);
}

static esp_err_t baro_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_baro, data, sizeof(data), 10);
}

static esp_err_t init_baro(void)
{
    ESP_LOGI(TAG, "barometer pins: SCL=%d SDA=%d, I2C 100kHz",
             BOARD_PIN_BARO_SCL, BOARD_PIN_BARO_SDA);
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_BARO_SDA,
        .scl_io_num = BOARD_PIN_BARO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c);
    if (err != ESP_OK) return err;
    const uint8_t addresses[] = {BOARD_BARO_I2C_ADDR, BOARD_BARO_I2C_ADDR ^ 1};
    s_baro = NULL;
    uint8_t selected_address = 0;
    for (unsigned index = 0; index < sizeof(addresses); index++) {
        err = i2c_master_probe(s_i2c, addresses[index], 10);
        ESP_LOGI(TAG, "I2C probe 0x%02x: %s, SCL=%d SDA=%d", addresses[index],
                 esp_err_to_name(err), gpio_get_level(BOARD_PIN_BARO_SCL),
                 gpio_get_level(BOARD_PIN_BARO_SDA));
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
        if (err != ESP_OK) continue;
        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[index],
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(s_i2c, &device_config, &s_baro);
        if (err != ESP_OK) return err;
        uint8_t id = 0;
        err = baro_read(0x0d, &id, 1);
        if (err == ESP_OK && id == BARO_CHIP_ID) {
            selected_address = addresses[index];
            break;
        }
        ESP_LOGW(TAG, "I2C 0x%02x ID=0x%02x, expected SPA06-003 0x11", addresses[index], id);
        i2c_master_bus_rm_device(s_baro);
        s_baro = NULL;
    }
    if (!s_baro) {
        unsigned found = 0;
        for (uint16_t address = 0x08; address < 0x76; address++) {
            err = i2c_master_probe(s_i2c, address, 10);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "I2C diagnostic ACK at 0x%02x (not identified as SPA06-003)", address);
                found++;
            } else if (err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "I2C diagnostic stopped at 0x%02x: %s", address, esp_err_to_name(err));
                return err;
            }
        }
        ESP_LOGW(TAG, "No SPA06-003 at 0x76/0x77; %u other I2C addresses ACK", found);
        return ESP_ERR_NOT_FOUND;
    }
    err = baro_write(0x0c, 0x09);
    if (err != ESP_OK) return err;
    int64_t deadline = esp_timer_get_time() + 200000;
    uint8_t status = 0;
    while ((status & 0xc0) != 0xc0) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(5));
        err = baro_read(0x08, &status, 1);
        if (err != ESP_OK) return err;
    }
    uint8_t raw_coefficients[21];
    err = baro_read(0x10, raw_coefficients, sizeof(raw_coefficients));
    if (err != ESP_OK) return err;
    if (!spa06_decode_coefficients(raw_coefficients, &s_coefficients)) return ESP_ERR_INVALID_RESPONSE;
    const uint8_t config[] = {0xc3, 0xc2, 0x07, 0x00};
    err = baro_write(0x06, config[0]);
    if (err == ESP_OK) err = baro_write(0x07, config[1]);
    if (err == ESP_OK) err = baro_write(0x09, config[3]);
    if (err == ESP_OK) err = baro_write(0x08, config[2]);
    if (err != ESP_OK) return err;
    uint8_t readback[4];
    err = baro_read(0x06, readback, sizeof(readback));
    if (err != ESP_OK) return err;
    if (readback[0] != config[0] || readback[1] != config[1] ||
        (readback[2] & 0x0f) != config[2] || readback[3] != config[3]) return ESP_ERR_INVALID_RESPONSE;
    ESP_LOGI(TAG, "SPA06-003 I2C 0x%02x: 25 Hz, pressure 8x, temperature 4x", selected_address);
    return ESP_OK;
}

void motion_init(void)
{
    esp_err_t err = init_imu();
    s_imu_ready = err == ESP_OK;
    if (!s_imu_ready) ESP_LOGW(TAG, "IMU unavailable: %s", esp_err_to_name(err));
    err = init_baro();
    s_baro_ready = err == ESP_OK;
    if (!s_baro_ready) ESP_LOGW(TAG, "barometer unavailable: %s", esp_err_to_name(err));
}

static esp_err_t sample_imu(sensors_reading_t *sample)
{
    if (!s_imu_ready) return ESP_ERR_INVALID_STATE;
    lsm6ds3tr_c_status_reg_t status;
    int32_t ret = lsm6ds3tr_c_status_reg_get(&s_imu_ctx, &status);
    if (ret != 0) {
        sample->imu_valid = false;
        return imu_status(ret);
    }
    if (!status.xlda || !status.gda) return ESP_OK;
    int16_t accel_raw[3] = {0};
    int16_t gyro_raw[3] = {0};
    ret = lsm6ds3tr_c_acceleration_raw_get(&s_imu_ctx, accel_raw);
    if (ret == 0) ret = lsm6ds3tr_c_angular_rate_raw_get(&s_imu_ctx, gyro_raw);
    if (ret != 0) {
        sample->imu_valid = false;
        return imu_status(ret);
    }
    for (unsigned axis = 0; axis < 3; axis++) {
        sample->accel_mg[axis] = (int16_t)lroundf(lsm6ds3tr_c_from_fs2g_to_mg(accel_raw[axis]));
        sample->gyro_mdps[axis] = (int32_t)lroundf(lsm6ds3tr_c_from_fs250dps_to_mdps(gyro_raw[axis]));
    }
    sample->imu_sample_us = esp_timer_get_time();
    sample->imu_valid = true;
    return ESP_OK;
}

static esp_err_t sample_baro(sensors_reading_t *sample)
{
    if (!s_baro_ready) return ESP_ERR_INVALID_STATE;
    uint8_t status = 0;
    esp_err_t err = baro_read(0x08, &status, 1);
    if (err != ESP_OK) goto failed;
    if (status & 0x20) {
        err = baro_read(0x03, s_baro_raw + 3, 3);
        if (err != ESP_OK) goto failed;
        s_temperature_us = esp_timer_get_time();
    }
    if (!(status & 0x10)) return ESP_OK;
    err = baro_read(0x00, s_baro_raw, 3);
    if (err != ESP_OK) goto failed;
    int64_t now = esp_timer_get_time();
    if (!s_temperature_us || now - s_temperature_us > BARO_MAX_AGE_US) {
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    if (!spa06_compensate(&s_coefficients, s_baro_raw, &sample->pressure_pa, &sample->temperature_mdegc)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto failed;
    }
    sample->baro_sample_us = now;
    sample->baro_valid = true;
    return ESP_OK;
failed:
    sample->baro_valid = false;
    return err;
}

esp_err_t motion_sample(void)
{
    sensors_reading_t sample;
    portENTER_CRITICAL(&s_lock);
    sample = s_sample;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t imu_err = sample_imu(&sample);
    esp_err_t baro_err = sample_baro(&sample);
    portENTER_CRITICAL(&s_lock);
    s_sample = sample;
    portEXIT_CRITICAL(&s_lock);
    return imu_err != ESP_OK ? imu_err : baro_err;
}

void motion_get(sensors_reading_t *out)
{
    portENTER_CRITICAL(&s_lock);
    out->imu_valid = s_sample.imu_valid;
    memcpy(out->accel_mg, s_sample.accel_mg, sizeof(out->accel_mg));
    memcpy(out->gyro_mdps, s_sample.gyro_mdps, sizeof(out->gyro_mdps));
    out->imu_sample_us = s_sample.imu_sample_us;
    out->baro_valid = s_sample.baro_valid;
    out->pressure_pa = s_sample.pressure_pa;
    out->temperature_mdegc = s_sample.temperature_mdegc;
    out->baro_sample_us = s_sample.baro_sample_us;
    portEXIT_CRITICAL(&s_lock);
    int64_t now = esp_timer_get_time();
    out->imu_valid &= out->imu_sample_us > 0 && now - out->imu_sample_us <= IMU_MAX_AGE_US;
    out->baro_valid &= out->baro_sample_us > 0 && now - out->baro_sample_us <= BARO_MAX_AGE_US;
}

/*
 * Bench diagnostics. Bus I/O runs outside the runtime lock, exactly as the sampling path
 * does, so a probe cannot stall the servo tick; the SPI and I2C master drivers serialise
 * concurrent transactions on their own.
 */

/* A temporary device handle lets a probe reach any address without touching the
 * barometer handle the driver owns - which is NULL when init rejected the chip. */
static esp_err_t add_temporary_device(uint8_t address, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(s_i2c, &config, out);
}

bool sensors_debug_imu_bus_ready(void)
{
    return s_imu != NULL;
}

bool sensors_debug_i2c_bus_ready(void)
{
    return s_i2c != NULL;
}

esp_err_t sensors_debug_imu_read(uint8_t reg, uint8_t *data, size_t length)
{
    if (!s_imu || !data || !length) return ESP_ERR_INVALID_STATE;
    return imu_status(imu_read(NULL, reg, data, (uint16_t)length));
}

esp_err_t sensors_debug_imu_write(uint8_t reg, uint8_t value)
{
    if (!s_imu) return ESP_ERR_INVALID_STATE;
    return imu_status(imu_write(NULL, reg, &value, 1));
}

esp_err_t sensors_debug_i2c_probe(uint8_t address)
{
    if (!s_i2c) return ESP_ERR_INVALID_STATE;
    return i2c_master_probe(s_i2c, address, 10);
}

esp_err_t sensors_debug_i2c_read(uint8_t address, uint8_t reg, uint8_t *data, size_t length)
{
    if (!s_i2c || !data || !length) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t device = NULL;
    esp_err_t err = add_temporary_device(address, &device);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(device, &reg, 1, data, length, 50);
    i2c_master_bus_rm_device(device);
    return err;
}

esp_err_t sensors_debug_i2c_write(uint8_t address, uint8_t reg, uint8_t value)
{
    if (!s_i2c) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t device = NULL;
    esp_err_t err = add_temporary_device(address, &device);
    if (err != ESP_OK) return err;
    uint8_t data[] = {reg, value};
    err = i2c_master_transmit(device, data, sizeof(data), 50);
    i2c_master_bus_rm_device(device);
    return err;
}

void sensors_debug_line_levels(int *imu_miso, int *i2c_scl, int *i2c_sda)
{
    if (imu_miso) *imu_miso = s_imu ? gpio_get_level(BOARD_PIN_IMU_MISO) : -1;
    if (i2c_scl) *i2c_scl = s_i2c ? gpio_get_level(BOARD_PIN_BARO_SCL) : -1;
    if (i2c_sda) *i2c_sda = s_i2c ? gpio_get_level(BOARD_PIN_BARO_SDA) : -1;
}
