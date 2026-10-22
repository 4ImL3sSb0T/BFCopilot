#pragma once

#include "lib/tools/common_def.h"
#include "esp_err.h"

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

/*
 * 连续采样的总速率。两个通道轮流采样，每个通道实际速率是它的一半。
 * 采样率越高，IIR 滤波的等效平滑窗口越短；这两个量要一起调。
 */
#define BSP_ADC_SAMPLE_FREQ_HZ 20000

/*
 * 芯片内置 IIR 滤波器系数，只允许 2/4/8/16/32/64。
 * 越大越平滑、响应越慢：10 kHz 采样下系数 16 约 1.6 ms 时间常数。
 */
#define BSP_ADC_IIR_FILTER_COEFF 16

typedef enum {
    BSP_ADC_VBAT,     // 电池电压分压
    BSP_ADC_ISERVO,   // 舵机电流
    BSP_ADC_COUNT,    // 仅用于边界检查
} BSP_ADC_CH;

/// @brief 初始化 ADC1 连续采样、硬件 IIR 滤波与校准
esp_err_t bsp_adc_init(void);

/// @brief 读最近一次滤波后的原始码值（0–4095）
/// @note 连续采样在后台持续运行，本函数只返回最新样本；任务上下文调用，不可在中断里使用
esp_err_t bsp_adc_read_raw(BSP_ADC_CH ch, u16 *raw);

/// @brief 读最近一次滤波后的引脚电压，单位 mV
/// @note 平滑由硬件 IIR 完成，本函数不再做软件平均；任务上下文调用，不可在中断里使用
esp_err_t bsp_adc_read_mv(BSP_ADC_CH ch, u16 *millivolt);
