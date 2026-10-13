#include <stdio.h>
#include <string.h>

#include "motion.h"
#include "board_pins.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

static int failures;
static int64_t now_us = 1000;
static uint8_t imu_registers[256], baro_registers[256];
static bool spi_error, i2c_error, stuck_reset, stuck_coefficients;
static uint8_t baro_address = 0x77;
static uint8_t probed_address;
static unsigned probe_count;

int gpio_get_level(int pin) { (void)pin; return 1; }

#define CHECK(condition) do { if (!(condition)) { \
    printf("FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ticks) { now_us += ticks * 1000; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock error"; }

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma)
{
    CHECK(host == SPI2_HOST && dma == SPI_DMA_DISABLED);
    CHECK(config->sclk_io_num == BOARD_PIN_IMU_SCK && config->mosi_io_num == BOARD_PIN_IMU_MOSI);
    CHECK(config->miso_io_num == BOARD_PIN_IMU_MISO);
    return ESP_OK;
}

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *device)
{
    (void)host;
    CHECK(config->mode == 3 && config->command_bits == 8);
    CHECK(config->spics_io_num == BOARD_PIN_IMU_CS);
    *device = imu_registers;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *transaction)
{
    (void)device;
    if (spi_error) return ESP_ERR_TIMEOUT;
    unsigned reg = transaction->cmd & 0x7f;
    unsigned length = transaction->length / 8;
    if (transaction->cmd & 0x80) {
        memcpy(transaction->rx_buffer, imu_registers + reg, length);
    } else {
        const uint8_t *source = transaction->tx_buffer ? transaction->tx_buffer : transaction->tx_data;
        for (unsigned index = 0; index < length; index++) {
            imu_registers[reg + index] = source[index];
        }
        /* the device clears SW_RESET itself once the reset has finished */
        if (reg == 0x12 && (imu_registers[reg] & 1) && !stuck_reset) imu_registers[reg] &= (uint8_t)~1;
    }
    return ESP_OK;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *bus)
{
    CHECK(config->scl_io_num == BOARD_PIN_BARO_SCL && config->sda_io_num == BOARD_PIN_BARO_SDA);
    *bus = baro_registers;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device)
{
    (void)bus;
    CHECK(config->scl_speed_hz == 100000);
    CHECK(config->device_address == baro_address);
    *device = baro_registers;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) { (void)device; return ESP_OK; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout)
{
    (void)bus;
    CHECK(timeout > 0);
    probe_count++;
    probed_address = (uint8_t)address;
    if (i2c_error) return ESP_ERR_TIMEOUT;
    return address == baro_address ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *command,
                                      size_t command_size, uint8_t *data, size_t length, int timeout)
{
    (void)device;
    CHECK(command_size == 1 && timeout > 0);
    if (i2c_error) return ESP_ERR_TIMEOUT;
    memcpy(data, baro_registers + command[0], length);
    if (command[0] == 0x00) baro_registers[8] &= (uint8_t)~0x10;
    if (command[0] == 0x03) baro_registers[8] &= (uint8_t)~0x20;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t length, int timeout)
{
    (void)device;
    CHECK(length == 2 && timeout > 0);
    if (i2c_error) return ESP_ERR_TIMEOUT;
    if (data[0] == 0x08) baro_registers[8] = (baro_registers[8] & 0xf0) | data[1];
    else baro_registers[data[0]] = data[1];
    if (data[0] == 0x0c) baro_registers[8] = stuck_coefficients ? 0 : 0xc0;
    return ESP_OK;
}

static void prepare_registers(void)
{
    memset(imu_registers, 0, sizeof(imu_registers));
    memset(baro_registers, 0, sizeof(baro_registers));
    imu_registers[0x0f] = 0x6a;
    baro_registers[0x0d] = 0x11;
    const uint8_t coefficients[] = {0x02, 0x80, 0x02, 0x18, 0x6a, 0x00, 0x03, 0xe8};
    memcpy(baro_registers + 0x10, coefficients, sizeof(coefficients));
}

int main(void)
{
    sensors_reading_t sample = {0};
    prepare_registers();
    imu_registers[0x0f] = 0x6b;
    motion_init();
    CHECK(motion_sample() == ESP_ERR_INVALID_STATE);
    motion_get(&sample);
    CHECK(!sample.imu_valid);

    prepare_registers();
    stuck_reset = true;
    stuck_coefficients = true;
    int64_t before_init = now_us;
    motion_init();
    CHECK(now_us - before_init >= 320000 && now_us - before_init < 340000);
    CHECK(motion_sample() == ESP_ERR_INVALID_STATE);
    stuck_reset = false;
    stuck_coefficients = false;

    prepare_registers();
    baro_address = 0;
    probe_count = 0;
    motion_init();
    CHECK(probe_count == 112);
    CHECK(motion_sample() == ESP_ERR_INVALID_STATE);

    prepare_registers();
    i2c_error = true;
    probe_count = 0;
    motion_init();
    CHECK(probe_count == 1);
    CHECK(motion_sample() == ESP_ERR_INVALID_STATE);
    i2c_error = false;

    prepare_registers();
    baro_address = 0x76;
    motion_init();
    CHECK(probed_address == 0x76);
    CHECK(imu_registers[0x10] == 0x40 && imu_registers[0x11] == 0x40 && imu_registers[0x12] == 0x44);
    CHECK(baro_registers[0x06] == 0xc3 && baro_registers[0x07] == 0xc2);
    CHECK(baro_registers[0x08] == 0xc7 && baro_registers[0x09] == 0);
    CHECK(motion_sample() == ESP_OK);
    motion_get(&sample);
    CHECK(!sample.imu_valid && !sample.baro_valid);

    imu_registers[0x1e] = 3;
    imu_registers[0x29] = 0x40;
    baro_registers[0x08] |= 0x20;
    CHECK(motion_sample() == ESP_OK);
    motion_get(&sample);
    CHECK(sample.imu_valid && sample.accel_mg[0] == 999 && !sample.baro_valid);
    baro_registers[0x08] |= 0x10;
    CHECK(motion_sample() == ESP_OK);
    motion_get(&sample);
    CHECK(sample.baro_valid && sample.pressure_pa == 100000 && sample.temperature_mdegc == 20000);

    /* known vectors through the ST driver conversions: +-2 g and +-250 dps full scale */
    const uint8_t imu_raw[12] = {0xff, 0x7f, 0x00, 0x80, 0x90, 0x01,
                                 0x00, 0x40, 0x00, 0xc0, 0x00, 0x00};
    memcpy(imu_registers + 0x22, imu_raw, 6);
    memcpy(imu_registers + 0x28, imu_raw + 6, 6);
    imu_registers[0x1e] = 3;
    CHECK(motion_sample() == ESP_OK);
    motion_get(&sample);
    CHECK(sample.accel_mg[0] == 999 && sample.accel_mg[1] == -999 && sample.accel_mg[2] == 0);
    CHECK(sample.gyro_mdps[0] == 286711 && sample.gyro_mdps[1] == -286720 &&
          sample.gyro_mdps[2] == 3500);

    imu_registers[0x1e] = 0;
    now_us += 100001;
    motion_get(&sample);
    CHECK(!sample.imu_valid && sample.baro_valid);
    now_us += 150000;
    motion_get(&sample);
    CHECK(!sample.baro_valid);
    baro_registers[0x08] |= 0x10;
    CHECK(motion_sample() == ESP_ERR_INVALID_STATE);

    imu_registers[0x1e] = 3;
    baro_registers[0x08] |= 0x30;
    CHECK(motion_sample() == ESP_OK);
    motion_get(&sample);
    CHECK(sample.imu_valid && sample.baro_valid);
    spi_error = true;
    i2c_error = true;
    CHECK(motion_sample() == ESP_ERR_TIMEOUT);
    motion_get(&sample);
    CHECK(!sample.imu_valid && !sample.baro_valid);
    spi_error = false;
    i2c_error = false;
    baro_registers[0x08] |= 0x30;
    CHECK(motion_sample() == ESP_OK);
    motion_get(&sample);
    CHECK(sample.imu_valid && sample.baro_valid);

    printf("motion driver tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
