#pragma once

#include "lib/tools/common_def.h"

/*
 * 模拟采样通道。
 *
 * 引脚来自原理图 P1（2026-10-20）：
 *   GPIO0 / ADC1_CH0 —— 电池分压（R13 20K / R14 10K，R15 串入 ADC）
 *   GPIO1 / ADC1_CH1 —— 舵机电流（INA199A1 OUT 经 R12 10K）
 *
 * 12 dB 衰减，单端量程约 0–3.1 V。BSP 只返回引脚电压，分压比、采样电阻和
 * 增益都是未标定参数，换算放在参数层，绝不在这里写死。
 */

#define BSP_ADC_VBAT_GPIO      0
#define BSP_ADC_ISERVO_GPIO    1

typedef enum {
    BSP_ADC_VBAT,     // 电池电压分压
    BSP_ADC_ISERVO,   // 舵机电流
    BSP_ADC_COUNT,    // 仅用于边界检查
} BSP_ADC_CH;

/// @brief 初始化 ADC1 单次转换与校准
exit_code_t bsp_adc_init(void);

/// @brief 读一次原始码值（0–4095）
/// @note 任务上下文调用，不可在中断里使用
exit_code_t bsp_adc_read_raw(BSP_ADC_CH ch, u16 *raw);

/// @brief 读滤波后的引脚电压，单位 mV
/// @note 内部平均若干次单次转换；任务上下文调用，不可在中断里使用
exit_code_t bsp_adc_read_mv(BSP_ADC_CH ch, u16 *millivolt);
