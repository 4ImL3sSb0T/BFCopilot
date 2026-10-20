#include "bsp_baro.h"

#include "spa06_math.h"

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BSP_BARO_I2C_PORT           I2C_NUM_0
#define BSP_BARO_TEMP_MAX_AGE_US    250000
#define BSP_BARO_READY_TIMEOUT_US   200000

// SPA06-003 寄存器（数据手册）
#define SPA06_REG_PRS_B2       0x00
#define SPA06_REG_TMP_B2       0x03
#define SPA06_REG_PRS_CFG      0x06
#define SPA06_REG_TMP_CFG      0x07
#define SPA06_REG_MEAS_CFG     0x08
#define SPA06_REG_CFG_REG      0x09
#define SPA06_REG_RESET        0x0c
#define SPA06_REG_ID           0x0d
#define SPA06_REG_COEF         0x10

#define SPA06_STATUS_COEF_RDY  0x80
#define SPA06_STATUS_SENSOR_RDY 0x40
#define SPA06_STATUS_TMP_RDY   0x20
#define SPA06_STATUS_PRS_RDY   0x10

// 连续测量：压力 8x、温度 4x 过采样（与 spa06_math.c 的缩放系数配套）
#define SPA06_PRS_CFG_VALUE    0xc3
#define SPA06_TMP_CFG_VALUE    0xc2
#define SPA06_MEAS_CFG_VALUE   0x07
#define SPA06_CFG_REG_VALUE    0x00

static const char *TAG = "bsp_baro";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static spa06_coefficients_t s_coefficients;
static u8 s_raw[6];
static i64 s_temperature_us;
static u8 s_address;
static bool s_ready;

static exit_code_t bsp_baro_status(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return EXIT_OK;
    case ESP_ERR_INVALID_ARG:
        return EXIT_INVALID_PARAM;
    case ESP_ERR_INVALID_STATE:
        return EXIT_NOT_INITIALIZED;
    case ESP_ERR_NO_MEM:
        return EXIT_NO_MEMORY;
    case ESP_ERR_NOT_FOUND:
        return EXIT_DOES_NOT_EXIST;
    case ESP_ERR_NOT_SUPPORTED:
        return EXIT_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT:
        return EXIT_TIMEOUT;
    default:
        return EXIT_HW_FAILURE;
    }
}

static exit_code_t bsp_baro_read_reg(u8 reg, u8 *data, size_t length) {
    return bsp_baro_status(i2c_master_transmit_receive(
        s_dev, &reg, 1, data, length, BSP_BARO_I2C_TIMEOUT_MS));
}

static exit_code_t bsp_baro_write_reg(u8 reg, u8 value) {
    const u8 data[2] = {reg, value};
    return bsp_baro_status(i2c_master_transmit(
        s_dev, data, sizeof(data), BSP_BARO_I2C_TIMEOUT_MS));
}

static void bsp_baro_release(void) {
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_address = 0;
}

exit_code_t bsp_baro_init(void) {
    if (s_ready) {
        return EXIT_ALREADY_INITIALIZED;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = BSP_BARO_I2C_PORT,
        .sda_io_num = BSP_BARO_PIN_SDA,
        .scl_io_num = BSP_BARO_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    exit_code_t rc = bsp_baro_status(i2c_new_master_bus(&bus_config, &s_bus));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return rc;
    }

    // SDO 悬空，地址不确定：两个候选都探测，并且用 ID 寄存器确认身份，
    // 不能只要有 ACK 就当成 SPA06。
    const u8 candidates[] = {BSP_BARO_ADDR_PRIMARY, BSP_BARO_ADDR_SECONDARY};
    for (unsigned index = 0; index < sizeof(candidates); index++) {
        const u8 address = candidates[index];
        if (i2c_master_probe(s_bus, address, BSP_BARO_I2C_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = BSP_BARO_I2C_CLOCK_HZ,
        };
        if (i2c_master_bus_add_device(s_bus, &device_config, &s_dev) != ESP_OK) {
            s_dev = NULL;
            continue;
        }
        u8 id = 0;
        if (bsp_baro_read_reg(SPA06_REG_ID, &id, 1) == EXIT_OK && id == BSP_BARO_CHIP_ID) {
            s_address = address;
            break;
        }
        ESP_LOGW(TAG, "I2C 0x%02x answered but ID=0x%02x (expected 0x%02x)",
                 address, id, BSP_BARO_CHIP_ID);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_dev == NULL) {
        ESP_LOGE(TAG, "no SPA06-003 at 0x%02x/0x%02x",
                 BSP_BARO_ADDR_PRIMARY, BSP_BARO_ADDR_SECONDARY);
        bsp_baro_release();
        return EXIT_DOES_NOT_EXIST;
    }

    // 软复位后等系数和传感器就绪
    rc = bsp_baro_write_reg(SPA06_REG_RESET, 0x09);
    if (rc == EXIT_OK) {
        const i64 deadline = esp_timer_get_time() + BSP_BARO_READY_TIMEOUT_US;
        u8 status = 0;
        while ((status & (SPA06_STATUS_COEF_RDY | SPA06_STATUS_SENSOR_RDY)) !=
               (SPA06_STATUS_COEF_RDY | SPA06_STATUS_SENSOR_RDY)) {
            if (esp_timer_get_time() >= deadline) {
                rc = EXIT_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            rc = bsp_baro_read_reg(SPA06_REG_MEAS_CFG, &status, 1);
            if (rc != EXIT_OK) {
                break;
            }
        }
    }

    u8 raw_coefficients[21];
    if (rc == EXIT_OK) {
        rc = bsp_baro_read_reg(SPA06_REG_COEF, raw_coefficients, sizeof(raw_coefficients));
    }
    if (rc == EXIT_OK && !spa06_decode_coefficients(raw_coefficients, &s_coefficients)) {
        ESP_LOGE(TAG, "coefficient block is invalid");
        rc = EXIT_HW_FAILURE;
    }
    if (rc != EXIT_OK) {
        bsp_baro_release();
        return rc;
    }

    rc = bsp_baro_write_reg(SPA06_REG_PRS_CFG, SPA06_PRS_CFG_VALUE);
    if (rc == EXIT_OK) rc = bsp_baro_write_reg(SPA06_REG_TMP_CFG, SPA06_TMP_CFG_VALUE);
    if (rc == EXIT_OK) rc = bsp_baro_write_reg(SPA06_REG_CFG_REG, SPA06_CFG_REG_VALUE);
    if (rc == EXIT_OK) rc = bsp_baro_write_reg(SPA06_REG_MEAS_CFG, SPA06_MEAS_CFG_VALUE);
    if (rc != EXIT_OK) {
        bsp_baro_release();
        return rc;
    }

    u8 readback[4] = {0};
    rc = bsp_baro_read_reg(SPA06_REG_PRS_CFG, readback, sizeof(readback));
    if (rc != EXIT_OK ||
        readback[0] != SPA06_PRS_CFG_VALUE ||
        readback[1] != SPA06_TMP_CFG_VALUE ||
        (readback[2] & 0x0f) != SPA06_MEAS_CFG_VALUE ||
        readback[3] != SPA06_CFG_REG_VALUE) {
        ESP_LOGE(TAG, "configuration readback mismatch: %02x %02x %02x %02x",
                 readback[0], readback[1], readback[2], readback[3]);
        bsp_baro_release();
        return EXIT_HW_FAILURE;
    }

    s_temperature_us = 0;
    s_ready = true;
    ESP_LOGI(TAG, "SPA06-003 ready at I2C 0x%02x (SCL=GPIO%d SDA=GPIO%d, continuous)",
             s_address, BSP_BARO_PIN_SCL, BSP_BARO_PIN_SDA);
    return EXIT_OK;
}

bool bsp_baro_is_ready(void) {
    return s_ready;
}

u8 bsp_baro_address(void) {
    return s_ready ? s_address : 0;
}

exit_code_t bsp_baro_read(bsp_baro_sample_t *sample) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (sample == NULL) {
        return EXIT_INVALID_PARAM;
    }

    u8 status = 0;
    exit_code_t rc = bsp_baro_read_reg(SPA06_REG_MEAS_CFG, &status, 1);
    if (rc != EXIT_OK) {
        return rc;
    }

    if (status & SPA06_STATUS_TMP_RDY) {
        rc = bsp_baro_read_reg(SPA06_REG_TMP_B2, s_raw + 3, 3);
        if (rc != EXIT_OK) {
            return rc;
        }
        s_temperature_us = esp_timer_get_time();
    }
    if (!(status & SPA06_STATUS_PRS_RDY)) {
        return EXIT_SKIP;
    }
    rc = bsp_baro_read_reg(SPA06_REG_PRS_B2, s_raw, 3);
    if (rc != EXIT_OK) {
        return rc;
    }
    // 补偿必须用较新的温度；温度过期就等下一轮，避免用旧温度算出错误的压力
    const i64 now = esp_timer_get_time();
    if (s_temperature_us == 0 || now - s_temperature_us > BSP_BARO_TEMP_MAX_AGE_US) {
        return EXIT_SKIP;
    }

    f32 pressure_pa = 0.0f;
    f32 temperature_c = 0.0f;
    if (!spa06_compensate(&s_coefficients, s_raw, &pressure_pa, &temperature_c)) {
        return EXIT_HW_FAILURE;
    }
    sample->pressure_pa = pressure_pa;
    sample->temperature_c = temperature_c;
    return EXIT_OK;
}
