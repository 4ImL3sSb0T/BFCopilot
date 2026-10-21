#pragma once

#include "lib/tools/common_def.h"
#include "lib/tools/vec_math.h"
#include "esp_err.h"
#include <stdbool.h>

/*
 * IMU：U3 LSM6DS3TR-C，4 线 SPI。
 *
 * 原理图 P1（2026-10-20）：
 *   SCK=GPIO7  MOSI=GPIO15  MISO=GPIO14  CS=GPIO6
 *   SDx/SCx 接地（选择 SPI 模式），INT1/INT2 未使用。
 *
 * vendor/ 下是 ST 官方 PID 驱动 2.2.3（BSD-3-Clause），只做寄存器语义；
 * 本文件负责 SPI 总线、片选、复位和量程/速率配置。不要修改 vendor/。
 */

#define BSP_IMU_PIN_SCK        7
#define BSP_IMU_PIN_MOSI       15
#define BSP_IMU_PIN_MISO       14
#define BSP_IMU_PIN_CS         6
#define BSP_IMU_SPI_CLOCK_HZ   1000000
#define BSP_IMU_SPI_MODE       3

typedef struct {
    vec3f accel_g;        // 加速度，单位 g
    vec3f gyro_dps;       // 角速度，单位 °/s
    f32 temperature_c;    // 芯片温度，单位 °C
} bsp_imu_sample_t;

/// @brief 初始化 SPI 总线、校验 WHO_AM_I 并配置传感器
/// @note 任务上下文调用；失败时会释放已申请的 SPI 资源
esp_err_t bsp_imu_init(void);

/// @brief 返回传感器是否已成功初始化
bool bsp_imu_is_ready(void);

/// @brief 读 WHO_AM_I（0x0F），预期 0x6A
esp_err_t bsp_imu_probe(u8 *who_am_i);

/// @brief 读一帧加速度/角速度/温度
/// @return ESP_OK 有新数据；ESP_ERR_NOT_FINISHED 本周期无新数据（sample 不变）
esp_err_t bsp_imu_read(bsp_imu_sample_t *sample);
