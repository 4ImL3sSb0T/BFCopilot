#pragma once

#include "lib/tools/common_def.h"
#include "esp_err.h"
#include <stdbool.h>

/*
 * 舵机输出：MCPWM 双通道，200 Hz，1 µs 分辨率。
 *
 * 原理图 P1（2026-10-20）：
 *   H1-3 ← GPIO18（左），H2-3 ← GPIO19（右）
 *   H1-2 / H2-2 = VCC_Servo，由 GPIO2(SERVO_EN) 经 Q3→Q2 高边开关控制
 *
 * 安全约定：
 *   - 初始化后两路输出被强制拉低，没有脉冲；必须显式 output_enable(true)
 *     才会输出。上电不会自己动舵机。
 *   - 舵机电源默认关断。关断电源时会同时把两路输出压回低电平。
 *   - BSP_SERVO_PULSE_MIN/MAX_US 是机械保护硬限，不是标定值；真正的
 *     舵机行程端点仍由参数层给出（calibrated=0 时不得用于飞行）。
 */

#define BSP_SERVO_LEFT_GPIO         18
#define BSP_SERVO_RIGHT_GPIO        19
#define BSP_SERVO_POWER_GPIO        2

#define BSP_SERVO_PWM_FREQ_HZ       200
/* 周期由频率推出（1 µs 分辨率）：200 Hz → 5000 µs，避免两个宏各自漂移 */
#define BSP_SERVO_PERIOD_US         (1000000 / BSP_SERVO_PWM_FREQ_HZ)
#define BSP_SERVO_PULSE_MIN_US      500
#define BSP_SERVO_PULSE_MAX_US      2500
#define BSP_SERVO_PULSE_NEUTRAL_US  1500

typedef enum {
    BSP_SERVO_LEFT,
    BSP_SERVO_RIGHT,
    BSP_SERVO_COUNT,
} BSP_SERVO_CH;

/// @brief 初始化 MCPWM 与舵机电源引脚（电源保持关断）
esp_err_t bsp_servo_init(void);

/// @brief 设定脉宽（µs），超出硬限会被夹到 [MIN, MAX]
/// @note 只改比较值，不解除输出强制低电平；需要 output_enable 才真正出波形
esp_err_t bsp_servo_set_pulse_us(BSP_SERVO_CH ch, u16 pulse_us);

/// @brief 使能/关闭某一路的输出
/// @param enable true 输出 PWM；false 强制低电平（无脉冲）
esp_err_t bsp_servo_output_enable(BSP_SERVO_CH ch, bool enable);

/// @brief 开关舵机电源（GPIO2 高有效）
/// @note 关断时会把两路输出同时压回低电平
esp_err_t bsp_servo_power_enable(bool enable);

/// @brief 查询舵机电源当前状态
bool bsp_servo_power_enabled(void);
