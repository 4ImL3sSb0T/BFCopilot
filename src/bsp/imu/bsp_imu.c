#include "bsp_imu.h"

#include "vendor/lsm6ds3tr-c_reg.h"

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 采样配置。改量程时必须同时改下面的换算函数，否则物理量会差一个系数。
#define BSP_IMU_XL_ODR          LSM6DS3TR_C_XL_ODR_104Hz
#define BSP_IMU_GY_ODR          LSM6DS3TR_C_GY_ODR_104Hz
#define BSP_IMU_XL_FS           LSM6DS3TR_C_2g
#define BSP_IMU_GY_FS           LSM6DS3TR_C_250dps
#define BSP_IMU_RESET_TIMEOUT_US 100000

static const char *TAG = "bsp_imu";
static spi_device_handle_t s_spi;
static stmdev_ctx_t s_ctx;
static bool s_ready;

static exit_code_t bsp_imu_status(esp_err_t err) {
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

/*
 * ST 驱动只做寄存器语义，总线由这里提供。SPI 读要置地址最高位（0x80）；
 * ST 的参考例程用手动 CS 横跨"发地址/收数据"两次调用，而 ESP-IDF 的硬件 CS
 * 每个事务结束都会释放，所以把地址放进 command phase、数据放进同一事务的
 * rx/tx 阶段，一次调用内 CS 保持有效。
 */
static int32_t bsp_imu_spi_read(void *handle, uint8_t reg, uint8_t *data, uint16_t length) {
    (void)handle;
    spi_transaction_t transaction = {
        .cmd = (uint16_t)(reg | 0x80),
        .length = (size_t)length * 8,
        .rx_buffer = data,
    };
    return (int32_t)spi_device_polling_transmit(s_spi, &transaction);
}

static int32_t bsp_imu_spi_write(void *handle, uint8_t reg, const uint8_t *data, uint16_t length) {
    (void)handle;
    spi_transaction_t transaction = {
        .cmd = reg,
        .length = (size_t)length * 8,
        .tx_buffer = data,
    };
    return (int32_t)spi_device_polling_transmit(s_spi, &transaction);
}

static void bsp_imu_spi_delay(uint32_t millisec) {
    vTaskDelay(pdMS_TO_TICKS(millisec));
}

static exit_code_t bsp_imu_verify_config(void) {
    lsm6ds3tr_c_odr_xl_t xl_odr;
    lsm6ds3tr_c_odr_g_t gy_odr;
    lsm6ds3tr_c_fs_xl_t xl_full_scale;
    lsm6ds3tr_c_fs_g_t gy_full_scale;
    uint8_t block_update = 0;
    uint8_t auto_increment = 0;

    int32_t ret = lsm6ds3tr_c_xl_data_rate_get(&s_ctx, &xl_odr);
    if (ret == 0) ret = lsm6ds3tr_c_gy_data_rate_get(&s_ctx, &gy_odr);
    if (ret == 0) ret = lsm6ds3tr_c_xl_full_scale_get(&s_ctx, &xl_full_scale);
    if (ret == 0) ret = lsm6ds3tr_c_gy_full_scale_get(&s_ctx, &gy_full_scale);
    if (ret == 0) ret = lsm6ds3tr_c_block_data_update_get(&s_ctx, &block_update);
    if (ret == 0) ret = lsm6ds3tr_c_auto_increment_get(&s_ctx, &auto_increment);
    if (ret != 0) {
        return EXIT_HW_FAILURE;
    }
    if (xl_odr != BSP_IMU_XL_ODR || gy_odr != BSP_IMU_GY_ODR ||
        xl_full_scale != BSP_IMU_XL_FS || gy_full_scale != BSP_IMU_GY_FS ||
        block_update != PROPERTY_ENABLE || auto_increment != PROPERTY_ENABLE) {
        return EXIT_HW_FAILURE;
    }
    return EXIT_OK;
}

exit_code_t bsp_imu_init(void) {
    if (s_ready) {
        return EXIT_ALREADY_INITIALIZED;
    }

    ESP_LOGI(TAG, "pins: SCK=%d MOSI=%d MISO=%d CS=%d, SPI mode %d %d Hz",
             BSP_IMU_PIN_SCK, BSP_IMU_PIN_MOSI, BSP_IMU_PIN_MISO, BSP_IMU_PIN_CS,
             BSP_IMU_SPI_MODE, BSP_IMU_SPI_CLOCK_HZ);

    spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_IMU_PIN_MOSI,
        .miso_io_num = BSP_IMU_PIN_MISO,
        .sclk_io_num = BSP_IMU_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    exit_code_t rc = bsp_imu_status(
        spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_DISABLED));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "spi bus init failed");
        return rc;
    }

    spi_device_interface_config_t device_config = {
        .command_bits = 8,   // 8 位寄存器地址走 command phase，CS 全程有效
        .clock_speed_hz = BSP_IMU_SPI_CLOCK_HZ,
        .mode = BSP_IMU_SPI_MODE,
        .spics_io_num = BSP_IMU_PIN_CS,
        .queue_size = 4,
    };
    rc = bsp_imu_status(
        spi_bus_add_device(SPI2_HOST, &device_config, &s_spi));
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "spi device add failed");
        spi_bus_free(SPI2_HOST);
        return rc;
    }
    // 上电后给传感器一点时间完成内部启动
    vTaskDelay(pdMS_TO_TICKS(20));

    s_ctx.write_reg = bsp_imu_spi_write;
    s_ctx.read_reg = bsp_imu_spi_read;
    s_ctx.mdelay = bsp_imu_spi_delay;
    s_ctx.handle = NULL;

    u8 id = 0;
    int32_t ret = -1;
    for (u32 attempt = 0; attempt < 3; attempt++) {
        ret = lsm6ds3tr_c_device_id_get(&s_ctx, &id);
        if (ret == 0 && id == LSM6DS3TR_C_ID) {
            break;
        }
        if (attempt < 2) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (ret != 0 || id != LSM6DS3TR_C_ID) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02x (expected 0x%02x)", id, LSM6DS3TR_C_ID);
        spi_bus_remove_device(s_spi);
        spi_bus_free(SPI2_HOST);
        s_spi = NULL;
        return EXIT_HW_FAILURE;
    }

    ret = lsm6ds3tr_c_reset_set(&s_ctx, PROPERTY_ENABLE);
    if (ret == 0) {
        const int64_t deadline = esp_timer_get_time() + BSP_IMU_RESET_TIMEOUT_US;
        uint8_t reset = 1;
        while (reset) {
            if (esp_timer_get_time() >= deadline) {
                ret = -1;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            ret = lsm6ds3tr_c_reset_get(&s_ctx, &reset);
        }
    }
    if (ret == 0) ret = lsm6ds3tr_c_block_data_update_set(&s_ctx, PROPERTY_ENABLE);
    if (ret == 0) ret = lsm6ds3tr_c_auto_increment_set(&s_ctx, PROPERTY_ENABLE);
    if (ret == 0) ret = lsm6ds3tr_c_xl_data_rate_set(&s_ctx, BSP_IMU_XL_ODR);
    if (ret == 0) ret = lsm6ds3tr_c_gy_data_rate_set(&s_ctx, BSP_IMU_GY_ODR);
    if (ret == 0) ret = lsm6ds3tr_c_xl_full_scale_set(&s_ctx, BSP_IMU_XL_FS);
    if (ret == 0) ret = lsm6ds3tr_c_gy_full_scale_set(&s_ctx, BSP_IMU_GY_FS);
    if (ret != 0) {
        ESP_LOGE(TAG, "sensor configuration failed");
        spi_bus_remove_device(s_spi);
        spi_bus_free(SPI2_HOST);
        s_spi = NULL;
        return EXIT_HW_FAILURE;
    }

    rc = bsp_imu_verify_config();
    if (rc != EXIT_OK) {
        ESP_LOGE(TAG, "configuration readback mismatch");
        spi_bus_remove_device(s_spi);
        spi_bus_free(SPI2_HOST);
        s_spi = NULL;
        return rc;
    }

    s_ready = true;
    ESP_LOGI(TAG, "LSM6DS3TR-C ready: 104 Hz, +/-2 g, +/-250 dps");
    return EXIT_OK;
}

bool bsp_imu_is_ready(void) {
    return s_ready;
}

exit_code_t bsp_imu_probe(u8 *who_am_i) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (who_am_i == NULL) {
        return EXIT_INVALID_PARAM;
    }
    uint8_t id = 0;
    if (lsm6ds3tr_c_device_id_get(&s_ctx, &id) != 0) {
        return EXIT_HW_FAILURE;
    }
    *who_am_i = id;
    return EXIT_OK;
}

exit_code_t bsp_imu_read(bsp_imu_sample_t *sample) {
    if (!s_ready) {
        return EXIT_NOT_INITIALIZED;
    }
    if (sample == NULL) {
        return EXIT_INVALID_PARAM;
    }

    lsm6ds3tr_c_status_reg_t status;
    if (lsm6ds3tr_c_status_reg_get(&s_ctx, &status) != 0) {
        return EXIT_HW_FAILURE;
    }
    // 两个都到位才读：加速度和角速度同周期更新，避免拼出半帧数据
    if (!status.xlda || !status.gda) {
        return EXIT_SKIP;
    }

    int16_t accel_raw[3] = {0};
    int16_t gyro_raw[3] = {0};
    int16_t temperature_raw = 0;
    if (lsm6ds3tr_c_acceleration_raw_get(&s_ctx, accel_raw) != 0 ||
        lsm6ds3tr_c_angular_rate_raw_get(&s_ctx, gyro_raw) != 0 ||
        lsm6ds3tr_c_temperature_raw_get(&s_ctx, &temperature_raw) != 0) {
        return EXIT_HW_FAILURE;
    }

    // 量程固定为 ±2 g / ±250 dps，换算函数与 BSP_IMU_XL_FS/BSP_IMU_GY_FS 绑定
    sample->accel_g = vec3f_make(
        lsm6ds3tr_c_from_fs2g_to_mg(accel_raw[0]) / 1000.0f,
        lsm6ds3tr_c_from_fs2g_to_mg(accel_raw[1]) / 1000.0f,
        lsm6ds3tr_c_from_fs2g_to_mg(accel_raw[2]) / 1000.0f);
    sample->gyro_dps = vec3f_make(
        lsm6ds3tr_c_from_fs250dps_to_mdps(gyro_raw[0]) / 1000.0f,
        lsm6ds3tr_c_from_fs250dps_to_mdps(gyro_raw[1]) / 1000.0f,
        lsm6ds3tr_c_from_fs250dps_to_mdps(gyro_raw[2]) / 1000.0f);
    sample->temperature_c = lsm6ds3tr_c_from_lsb_to_celsius(temperature_raw);
    return EXIT_OK;
}
