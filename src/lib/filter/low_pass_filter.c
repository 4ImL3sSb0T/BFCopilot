#include "low_pass_filter.h"
#include <math.h>

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief 初始化一阶低通滤波器
 */
exit_code_t low_pass_filter_init(low_pass_filter_t *filter, float cutoff_freq, float sample_freq)
{
    if (filter == NULL) {
        return EXIT_INVALID_PARAM;
    }
    
    if (cutoff_freq <= 0.0f || sample_freq <= 0.0f || cutoff_freq >= sample_freq / 2.0f) {
        return EXIT_INVALID_PARAM;
    }
    
    // 计算滤波系数：α = 2πfdt / (2πfdt + 1)
    // 其中f为截止频率，dt = 1/sample_freq
    float dt = 1.0f / sample_freq;
    float rc = 1.0f / (2.0f * M_PI * cutoff_freq);
    filter->alpha = dt / (rc + dt);
    
    filter->output = 0.0f;
    filter->prev_output = 0.0f;
    filter->sample_count = 0;
    filter->state = FILTER_STATE_INITIALIZED;
    
    return EXIT_OK;
}

/**
 * @brief 使用滤波系数直接初始化一阶低通滤波器
 */
exit_code_t low_pass_filter_init_alpha(low_pass_filter_t *filter, float alpha)
{
    if (filter == NULL) {
        return EXIT_INVALID_PARAM;
    }
    
    if (alpha <= 0.0f || alpha > 1.0f) {
        return EXIT_INVALID_PARAM;
    }
    
    filter->alpha = alpha;
    filter->output = 0.0f;
    filter->prev_output = 0.0f;
    filter->sample_count = 0;
    filter->state = FILTER_STATE_INITIALIZED;
    
    return EXIT_OK;
}

/**
 * @brief 一阶低通滤波器处理单个数据
 */
filter_data_t low_pass_filter_update(low_pass_filter_t *filter, filter_data_t input)
{
    if (filter == NULL || filter->state == FILTER_STATE_ERROR) {
        return 0.0f;
    }
    
    if (filter->state == FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_ERROR;
        return 0.0f;
    }
    
    // 第一次输入时，直接设置为输入值
    if (filter->sample_count == 0) {
        filter->output = input;
        filter->prev_output = input;
        filter->state = FILTER_STATE_READY;
    } else {
        // 一阶低通滤波：Y(n) = α * X(n) + (1-α) * Y(n-1)
        filter->output = filter->alpha * input + (1.0f - filter->alpha) * filter->prev_output;
    }
    
    filter->prev_output = filter->output;
    filter->sample_count++;
    
    return filter->output;
}

/**
 * @brief 重置一阶低通滤波器
 */
void low_pass_filter_reset(low_pass_filter_t *filter, filter_data_t init_value)
{
    if (filter == NULL) {
        return;
    }
    
    filter->output = init_value;
    filter->prev_output = init_value;
    filter->sample_count = 0;
    
    if (filter->state != FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_INITIALIZED;
    }
}

/**
 * @brief 初始化二阶低通滤波器
 */
exit_code_t low_pass_filter_2nd_init(low_pass_filter_2nd_t *filter, 
                                      float cutoff_freq, float sample_freq, float damping)
{
    if (filter == NULL) {
        return EXIT_INVALID_PARAM;
    }
    
    if (cutoff_freq <= 0.0f || sample_freq <= 0.0f || cutoff_freq >= sample_freq / 2.0f) {
        return EXIT_INVALID_PARAM;
    }
    
    if (damping <= 0.0f) {
        return EXIT_INVALID_PARAM;
    }
    
    // 计算二阶Butterworth低通滤波器系数
    float dt = 1.0f / sample_freq;
    float wc = 2.0f * M_PI * cutoff_freq;  // 截止角频率
    float wc2 = wc * wc;
    float wc_dt = wc * dt;
    float wc2_dt2 = wc2 * dt * dt;
    
    // 二阶系统传递函数系数
    float a0 = 1.0f + 2.0f * damping * wc_dt + wc2_dt2;
    
    // 归一化系数
    filter->a1 = (2.0f * wc2_dt2 - 2.0f) / a0;
    filter->a2 = (1.0f - 2.0f * damping * wc_dt + wc2_dt2) / a0;
    
    filter->b0 = wc2_dt2 / a0;
    filter->b1 = 2.0f * wc2_dt2 / a0;
    filter->b2 = wc2_dt2 / a0;
    
    // 初始化状态变量
    filter->x1 = 0.0f;
    filter->x2 = 0.0f;
    filter->y1 = 0.0f;
    filter->y2 = 0.0f;
    filter->output = 0.0f;
    filter->sample_count = 0;
    filter->state = FILTER_STATE_INITIALIZED;
    
    return EXIT_OK;
}

/**
 * @brief 二阶低通滤波器处理单个数据
 */
filter_data_t low_pass_filter_2nd_update(low_pass_filter_2nd_t *filter, filter_data_t input)
{
    if (filter == NULL || filter->state == FILTER_STATE_ERROR) {
        return 0.0f;
    }
    
    if (filter->state == FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_ERROR;
        return 0.0f;
    }
    
    // 第一次输入时，直接设置为输入值
    if (filter->sample_count == 0) {
        filter->output = input;
        filter->x1 = input;
        filter->x2 = input;
        filter->y1 = input;
        filter->y2 = input;
        filter->state = FILTER_STATE_READY;
    } else {
        // 二阶IIR滤波器差分方程
        // y(n) = b0*x(n) + b1*x(n-1) + b2*x(n-2) - a1*y(n-1) - a2*y(n-2)
        filter->output = filter->b0 * input + 
                        filter->b1 * filter->x1 + 
                        filter->b2 * filter->x2 - 
                        filter->a1 * filter->y1 - 
                        filter->a2 * filter->y2;
    }
    
    // 更新历史状态
    filter->x2 = filter->x1;
    filter->x1 = input;
    filter->y2 = filter->y1;
    filter->y1 = filter->output;
    
    filter->sample_count++;
    
    return filter->output;
}

/**
 * @brief 重置二阶低通滤波器
 */
void low_pass_filter_2nd_reset(low_pass_filter_2nd_t *filter, filter_data_t init_value)
{
    if (filter == NULL) {
        return;
    }
    
    filter->x1 = init_value;
    filter->x2 = init_value;
    filter->y1 = init_value;
    filter->y2 = init_value;
    filter->output = init_value;
    filter->sample_count = 0;
    
    if (filter->state != FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_INITIALIZED;
    }
}

/**
 * @brief 获取一阶滤波器状态
 */
filter_state_t low_pass_filter_get_state(const low_pass_filter_t *filter)
{
    if (filter == NULL) {
        return FILTER_STATE_ERROR;
    }
    return filter->state;
}

/**
 * @brief 获取二阶滤波器状态
 */
filter_state_t low_pass_filter_2nd_get_state(const low_pass_filter_2nd_t *filter)
{
    if (filter == NULL) {
        return FILTER_STATE_ERROR;
    }
    return filter->state;
}

#ifdef USE_CMSIS_DSP
/**
 * @brief 初始化 DSP Biquad 低通滤波器
 */
exit_code_t low_pass_filter_biquad_init(low_pass_filter_biquad_t *filter, 
                                          float cutoff_freq, float sample_freq, float q_factor)
{
    if (filter == NULL) {
        return EXIT_INVALID_PARAM;
    }
    
    if (cutoff_freq <= 0.0f || sample_freq <= 0.0f || cutoff_freq >= sample_freq / 2.0f) {
        return EXIT_INVALID_PARAM;
    }
    
    if (q_factor <= 0.0f) {
        return EXIT_INVALID_PARAM;
    }
    
    // 计算 Biquad 系数 (Butterworth 低通)
    float w0 = 2.0f * M_PI * cutoff_freq / sample_freq;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * q_factor);
    
    // 标准 Biquad 系数
    float b0_raw = (1.0f - cos_w0) / 2.0f;
    float b1_raw = 1.0f - cos_w0;
    float b2_raw = (1.0f - cos_w0) / 2.0f;
    float a0 = 1.0f + alpha;
    float a1_raw = -2.0f * cos_w0;
    float a2_raw = 1.0f - alpha;
    
    // 归一化（除以 a0）并按 CMSIS-DSP 格式存储
    // CMSIS-DSP 格式：{b0, b1, b2, -a1, -a2}
    filter->coeffs[0] = b0_raw / a0;
    filter->coeffs[1] = b1_raw / a0;
    filter->coeffs[2] = b2_raw / a0;
    filter->coeffs[3] = -a1_raw / a0;  // 注意负号
    filter->coeffs[4] = -a2_raw / a0;  // 注意负号
    
    // 初始化状态缓冲区
    for (int i = 0; i < 4; i++) {
        filter->state[i] = 0.0f;
    }
    
    // 初始化 CMSIS-DSP Biquad 实例（1 级联）
    arm_biquad_cascade_df1_init_f32(&filter->instance, 1, filter->coeffs, filter->state);
    
    filter->output = 0.0f;
    filter->sample_count = 0;
    filter->filter_state = FILTER_STATE_INITIALIZED;
    
    return EXIT_OK;
}

/**
 * @brief DSP Biquad 低通滤波器处理单个数据
 */
filter_data_t low_pass_filter_biquad_update(low_pass_filter_biquad_t *filter, filter_data_t input)
{
    if (filter == NULL || filter->filter_state == FILTER_STATE_ERROR) {
        return 0.0f;
    }
    
    if (filter->filter_state == FILTER_STATE_UNINITIALIZED) {
        filter->filter_state = FILTER_STATE_ERROR;
        return 0.0f;
    }
    
    // 使用 CMSIS-DSP 处理（硬件加速）
    arm_biquad_cascade_df1_f32(&filter->instance, &input, &filter->output, 1);
    
    filter->sample_count++;
    filter->filter_state = FILTER_STATE_READY;
    
    return filter->output;
}

/**
 * @brief 重置 DSP Biquad 低通滤波器
 */
void low_pass_filter_biquad_reset(low_pass_filter_biquad_t *filter, filter_data_t init_value)
{
    if (filter == NULL) {
        return;
    }
    
    // 重置状态缓冲区
    for (int i = 0; i < 4; i++) {
        filter->state[i] = 0.0f;
    }
    
    filter->output = init_value;
    filter->sample_count = 0;
    
    if (filter->filter_state != FILTER_STATE_UNINITIALIZED) {
        filter->filter_state = FILTER_STATE_INITIALIZED;
    }
}

/**
 * @brief 获取 DSP Biquad 滤波器状态
 */
filter_state_t low_pass_filter_biquad_get_state(const low_pass_filter_biquad_t *filter)
{
    if (filter == NULL) {
        return FILTER_STATE_ERROR;
    }
    return filter->filter_state;
}
#endif // USE_CMSIS_DSP
