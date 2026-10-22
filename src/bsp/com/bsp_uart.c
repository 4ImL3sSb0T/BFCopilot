#include "bsp_uart.h"

#include <string.h>

#include "sdkconfig.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define BSP_UART_HW_FIFO_LEN 128 // 驱动要求 software buffer 大于硬件 FIFO

static const char *TAG = "bsp_uart";

static int s_port = -1;

esp_err_t bsp_uart_init(const bsp_uart_config_t *config) {
    if (s_port >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    bsp_uart_config_t cfg = BSP_UART_CONFIG_DEFAULT();
    if (config != NULL) {
        cfg = *config;
    }
    if (cfg.port >= UART_NUM_MAX || cfg.baud_rate == 0 ||
        cfg.rx_buffer_size <= BSP_UART_HW_FIFO_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    const uart_config_t uart_config = {
        .baud_rate = (int)cfg.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config((uart_port_t)cfg.port, &uart_config);
    if (err == ESP_OK && (cfg.tx_gpio >= 0 || cfg.rx_gpio >= 0)) {
        err = uart_set_pin((uart_port_t)cfg.port, (int)cfg.tx_gpio, (int)cfg.rx_gpio,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        // 中断接收，rx 数据进驱动环形缓冲；tx_buffer_size=0 时发送直接写 FIFO。
        err = uart_driver_install((uart_port_t)cfg.port, cfg.rx_buffer_size,
                                  cfg.tx_buffer_size, 0, NULL, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart%u init failed: %s", (unsigned)cfg.port, esp_err_to_name(err));
        return err;
    }

#if CONFIG_ESP_CONSOLE_UART
    if (cfg.port == CONFIG_ESP_CONSOLE_UART_NUM) {
        // UART0 同时是 console：把 VFS 的读写切到驱动上，日志和 bsp_uart_send()
        // 就会共用驱动里的同一把 TX 锁。
        uart_vfs_dev_use_driver((int)cfg.port);
    }
#endif

    s_port = cfg.port;
    ESP_LOGI(TAG, "uart%u ready at %u baud, rx buffer %u",
             (unsigned)cfg.port, (unsigned)cfg.baud_rate, (unsigned)cfg.rx_buffer_size);
    return ESP_OK;
}

esp_err_t bsp_uart_deinit(void) {
    if (s_port < 0) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_ESP_CONSOLE_UART
    if (s_port == CONFIG_ESP_CONSOLE_UART_NUM) {
        // 恢复 console 直接读写 FIFO 的路径，否则删掉驱动后日志会写不出去。
        uart_vfs_dev_use_nonblocking(s_port);
    }
#endif

    const esp_err_t err = uart_driver_delete((uart_port_t)s_port);
    s_port = -1;
    return err;
}

bool bsp_uart_ready(void) {
    return s_port >= 0;
}

esp_err_t bsp_uart_send(const void *data, size_t len) {
    if (s_port < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = uart_write_bytes((uart_port_t)s_port, data, len);
    return written == (int)len ? ESP_OK : ESP_FAIL;
}

int bsp_uart_recv(void *buf, size_t len, u32 timeout_ms) {
    if (s_port < 0 || buf == NULL || len == 0) {
        return -1;
    }
    return uart_read_bytes((uart_port_t)s_port, buf, len, pdMS_TO_TICKS(timeout_ms));
}
