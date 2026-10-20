#ifndef __LOW_PASS_FILTER_H__
#define __LOW_PASS_FILTER_H__

#include "filter_common.h"

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 一阶低通滤波器结构体
 * 
 * 实现一阶RC低通滤波器：
 * Y(n) = α * X(n) + (1-α) * Y(n-1)
 * 其中α = dt / (RC + dt)，dt为采样时间，RC为时间常数
 */
typedef struct {
    filter_data_t alpha;        // 滤波系数 (0 < alpha <= 1)
    filter_data_t output;       // 滤波输出值
    filter_data_t prev_output;  // 上一次输出值
    filter_state_t state;       // 滤波器状态
    uint32 sample_count;        // 采样计数
} low_pass_filter_t;

/**
 * @brief 二阶低通滤波器结构体
 * 
 * 实现二阶IIR低通滤波器（Butterworth）
 * 更好的滤波效果，但计算量稍大
 */
typedef struct {
    filter_data_t a1, a2;      // 反馈系数
    filter_data_t b0, b1, b2;  // 前馈系数
    filter_data_t x1, x2;      // 输入历史值
    filter_data_t y1, y2;      // 输出历史值
    filter_data_t output;      // 当前输出
    filter_state_t state;      // 滤波器状态
    uint32 sample_count;       // 采样计数
} low_pass_filter_2nd_t;

#ifdef USE_CMSIS_DSP
/**
 * @brief DSP 加速的 Biquad 低通滤波器结构体
 * 
 * 使用 CMSIS-DSP 的 arm_biquad_cascade_df1_f32 实现
 * 提供更高性能的二阶 IIR 滤波器
 */
typedef struct {
    arm_biquad_casd_df1_inst_f32 instance;  // DSP Biquad 实例
    float state[4];                          // 状态缓冲区（4个状态变量）
    float coeffs[5];                         // 系数数组 [b0, b1, b2, a1, a2]
    filter_data_t output;                    // 当前输出
    filter_state_t filter_state;             // 滤波器状态
    uint32 sample_count;                     // 采样计数
} low_pass_filter_biquad_t;
#endif

/**
 * @brief 初始化一阶低通滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param cutoff_freq 截止频率 (Hz)
 * @param sample_freq 采样频率 (Hz)
 * @return exit_code_t 错误码
 */
exit_code_t low_pass_filter_init(low_pass_filter_t *filter, float cutoff_freq, float sample_freq);

/**
 * @brief 使用滤波系数直接初始化一阶低通滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param alpha 滤波系数 (0 < alpha <= 1)
 * @return exit_code_t 错误码
 */
exit_code_t low_pass_filter_init_alpha(low_pass_filter_t *filter, float alpha);

/**
 * @brief 一阶低通滤波器处理单个数据
 * 
 * @param filter 滤波器结构体指针
 * @param input 输入数据
 * @return filter_data_t 滤波后的输出
 */
filter_data_t low_pass_filter_update(low_pass_filter_t *filter, filter_data_t input);

/**
 * @brief 重置一阶低通滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param init_value 初始值
 */
void low_pass_filter_reset(low_pass_filter_t *filter, filter_data_t init_value);

/**
 * @brief 初始化二阶低通滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param cutoff_freq 截止频率 (Hz)
 * @param sample_freq 采样频率 (Hz)
 * @param damping 阻尼比 (建议0.707为临界阻尼)
 * @return exit_code_t 错误码
 */
exit_code_t low_pass_filter_2nd_init(low_pass_filter_2nd_t *filter, 
                                      float cutoff_freq, float sample_freq, float damping);

/**
 * @brief 二阶低通滤波器处理单个数据
 * 
 * @param filter 滤波器结构体指针
 * @param input 输入数据
 * @return filter_data_t 滤波后的输出
 */
filter_data_t low_pass_filter_2nd_update(low_pass_filter_2nd_t *filter, filter_data_t input);

/**
 * @brief 重置二阶低通滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param init_value 初始值
 */
void low_pass_filter_2nd_reset(low_pass_filter_2nd_t *filter, filter_data_t init_value);

/**
 * @brief 获取一阶滤波器状态
 * 
 * @param filter 滤波器结构体指针
 * @return filter_state_t 滤波器状态
 */
filter_state_t low_pass_filter_get_state(const low_pass_filter_t *filter);

/**
 * @brief 获取二阶滤波器状态
 * 
 * @param filter 滤波器结构体指针
 * @return filter_state_t 滤波器状态
 */
filter_state_t low_pass_filter_2nd_get_state(const low_pass_filter_2nd_t *filter);

#ifdef USE_CMSIS_DSP
/**
 * @brief 初始化 DSP Biquad 低通滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param cutoff_freq 截止频率 (Hz)
 * @param sample_freq 采样频率 (Hz)
 * @param q_factor 品质因数 (推荐 0.707 为 Butterworth 响应)
 * @return exit_code_t 错误码
 */
exit_code_t low_pass_filter_biquad_init(low_pass_filter_biquad_t *filter, 
                                          float cutoff_freq, float sample_freq, float q_factor);

/**
 * @brief DSP Biquad 低通滤波器处理单个数据
 * 
 * @param filter 滤波器结构体指针
 * @param input 输入数据
 * @return filter_data_t 滤波后的输出
 */
filter_data_t low_pass_filter_biquad_update(low_pass_filter_biquad_t *filter, filter_data_t input);

/**
 * @brief 重置 DSP Biquad 低通滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param init_value 初始值
 */
void low_pass_filter_biquad_reset(low_pass_filter_biquad_t *filter, filter_data_t init_value);

/**
 * @brief 获取 DSP Biquad 滤波器状态
 * 
 * @param filter 滤波器结构体指针
 * @return filter_state_t 滤波器状态
 */
filter_state_t low_pass_filter_biquad_get_state(const low_pass_filter_biquad_t *filter);
#endif // USE_CMSIS_DSP

#ifdef __cplusplus
}
#endif

#endif // !__LOW_PASS_FILTER_H__