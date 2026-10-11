#include "motion.h"
#include "motion_math.h"
#include "board_pins.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define IMU_WHO_AM_I 0x6a
#define BARO_CHIP_ID 0x11
#define IMU_MAX_AGE_US 100000
#define BARO_MAX_AGE_US 250000

static const char *TAG = "motion";
static spi_device_handle_t s_imu;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_baro;
static bool s_imu_ready;
static bool s_baro_ready;
static spa06_coefficients_t s_coefficients;
static sensors_reading_t s_sample;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_baro_raw[6];
static int64_t s_temperature_us;

static esp_err_t imu_read(uint8_t reg, uint8_t *data, size_t length)
{
    spi_transaction_t transaction = {
        .cmd = reg | 0x80,
        .length = length * 8,
        .rx_buffer = data,
    };
    return spi_device_polling_transmit(s_imu, &transaction);
}

static esp_err_t imu_write(uint8_t reg, uint8_t value)
{
    spi_transaction_t transaction = {
        .cmd = reg,
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {value},
    };
    return spi_device_polling_transmit(s_imu, &transaction);
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
    uint8_t id = 0;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        err = imu_read(0x0f, &id, 1);
        if (err == ESP_OK && id == IMU_WHO_AM_I) break;
        ESP_LOGW(TAG, "IMU identity attempt %u: transfer=%s WHO_AM_I=0x%02x MISO=%d",
                 attempt + 1, esp_err_to_name(err), id, gpio_get_level(BOARD_PIN_IMU_MISO));
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK) return err;
    if (id != IMU_WHO_AM_I) {
        ESP_LOGW(TAG, "LSM6DS3TR-C WHO_AM_I=0x%02x, expected 0x%02x", id, IMU_WHO_AM_I);
        return ESP_ERR_NOT_FOUND;
    }
    err = imu_write(0x12, 0x01);
    if (err != ESP_OK) return err;
    int64_t deadline = esp_timer_get_time() + 100000;
    uint8_t control = 1;
    while (control & 1) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(1));
        err = imu_read(0x12, &control, 1);
        if (err != ESP_OK) return err;
    }
    const uint8_t config[] = {0x40, 0x40, 0x44};
    for (unsigned index = 0; index < sizeof(config); index++) {
        err = imu_write(0x10 + index, config[index]);
        if (err != ESP_OK) return err;
    }
    uint8_t readback[3];
    err = imu_read(0x10, readback, sizeof(readback));
    if (err != ESP_OK) return err;
    if (memcmp(config, readback, sizeof(config)) != 0) return ESP_ERR_INVALID_RESPONSE;
    ESP_LOGI(TAG, "LSM6DS3TR-C SPI: 104 Hz, +/-2 g, +/-250 dps, chip axes");
    return ESP_OK;
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
    uint8_t status = 0;
    esp_err_t err = imu_read(0x1e, &status, 1);
    if (err != ESP_OK) {
        sample->imu_valid = false;
        return err;
    }
    if ((status & 3) != 3) return ESP_OK;
    uint8_t raw[12];
    err = imu_read(0x22, raw, sizeof(raw));
    if (err != ESP_OK) {
        sample->imu_valid = false;
        return err;
    }
    lsm6ds3tr_decode(raw, sample->accel_mg, sample->gyro_mdps);
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
