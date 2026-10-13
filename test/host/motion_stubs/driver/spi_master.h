#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void *spi_device_handle_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    int command_bits, clock_speed_hz, mode, spics_io_num, queue_size;
} spi_device_interface_config_t;
typedef struct {
    uint16_t cmd;
    size_t length;
    uint32_t flags;
    uint8_t tx_data[4];
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;
#define SPI2_HOST 1
#define SPI_DMA_DISABLED 0
#define SPI_TRANS_USE_TXDATA 1
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma);
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *device);
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *transaction);
