#pragma once

#include "lib/tools/common_def.h"
#include "esp_err.h"

/*
 * 蜂鸣器：GPIO3 经 R10 1K 驱动 Q1(AO3400A_N) 低边开关，蜂鸣器由 3V3 供电。
 *
 * 器件是有源还是无源尚未确认（BOM 只写了 4 kHz），因此这里统一按 PWM 方波驱动：
 * 无源蜂鸣器按给定频率发声；有源蜂鸣器无论频率多少都会按自身谐振频率响。
 * 初始化后输出保持 0 占空比，复位期间不会发声。
 *
 * 注意：Q1 栅极没有下拉，复位/高阻期间可能被电荷顶开；上电后应尽早调用
 * bsp_buzzer_init()（或把 GPIO3 拉低）来消除这个不确定状态。
 */

#define BSP_BUZZER_GPIO            3
#define BSP_BUZZER_FREQ_DEFAULT_HZ 4000
// 下限 20 Hz 靠把 LEDC 分辨率提到 12 bit 换来：80 MHz APB / (1024 * 4096) ≈ 19.1 Hz
// 是分频器极限，再低只能换更低的时钟源。代价是上限跟着降到 80 MHz / 4096 ≈ 19.5 kHz，
// 已经贴着人耳上限，比原来的 20 kHz 听不出差别。
#define BSP_BUZZER_FREQ_MIN_HZ     20
#define BSP_BUZZER_FREQ_MAX_HZ     19500

/// @brief 初始化 LEDC 通道，输出静音
esp_err_t bsp_buzzer_init(void);

/// @brief 输出指定频率的 50% 方波
/// @param freq_hz 20–19500 Hz，超出范围返回 ESP_ERR_INVALID_ARG
esp_err_t bsp_buzzer_tone(u32 freq_hz);

/// @brief 停止发声（占空比归零）
esp_err_t bsp_buzzer_stop(void);
