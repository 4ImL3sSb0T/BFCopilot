#include "moving_average_filter.h"
#include <string.h>
#include <stdlib.h>

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

/**
 * @brief 比较函数，用于排序
 */
static int compare_float(const void *a, const void *b)
{
    filter_data_t fa = *(const filter_data_t*)a;
    filter_data_t fb = *(const filter_data_t*)b;
    
    if (fa < fb) return -1;
    if (fa > fb) return 1;
    return 0;
}

/**
 * @brief 初始化简单滑动均值滤波器
 */
exit_code_t moving_average_filter_init(moving_average_filter_t *filter, 
                                       filter_data_t *buffer, 
                                       uint16 window_size)
{
    if (filter == NULL || buffer == NULL || window_size == 0) {
        return EXIT_INVALID_PARAM;
    }
    
    filter->buffer = buffer;
    filter->window_size = window_size;
    filter->buffer_index = 0;
    filter->sample_count = 0;
    filter->sum = 0.0f;
    filter->output = 0.0f;
    filter->state = FILTER_STATE_INITIALIZED;
    
    // 清空缓冲区
    memset(buffer, 0, sizeof(filter_data_t) * window_size);
    
    return EXIT_OK;
}

/**
 * @brief 简单滑动均值滤波器处理单个数据
 */
filter_data_t moving_average_filter_update(moving_average_filter_t *filter, filter_data_t input)
{
    if (filter == NULL || filter->buffer == NULL || filter->state == FILTER_STATE_ERROR) {
        return 0.0f;
    }
    
    if (filter->state == FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_ERROR;
        return 0.0f;
    }
    
    // 如果缓冲区未满，直接添加数据
    if (filter->sample_count < filter->window_size) {
        filter->buffer[filter->buffer_index] = input;
        filter->sum += input;
        filter->sample_count++;
        filter->output = filter->sum / filter->sample_count;
    } else {
        // 缓冲区已满，替换最老的数据（保持 O(1) 更新，避免每次全窗口均值的 O(N) 开销）
        filter->sum = filter->sum - filter->buffer[filter->buffer_index] + input;
        filter->buffer[filter->buffer_index] = input;
        filter->output = filter->sum / filter->window_size;
    }
    
    // 更新索引
    filter->buffer_index++;
    if (filter->buffer_index >= filter->window_size) {
        filter->buffer_index = 0;
    }
    
    filter->state = FILTER_STATE_READY;
    
    return filter->output;
}

/**
 * @brief 重置简单滑动均值滤波器
 */
void moving_average_filter_reset(moving_average_filter_t *filter)
{
    if (filter == NULL || filter->buffer == NULL) {
        return;
    }
    
    filter->buffer_index = 0;
    filter->sample_count = 0;
    filter->sum = 0.0f;
    filter->output = 0.0f;
    
    if (filter->state != FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_INITIALIZED;
    }
    
    // 清空缓冲区
    memset(filter->buffer, 0, sizeof(filter_data_t) * filter->window_size);
}

/**
 * @brief 预填充简单滑动均值滤波器
 */
void moving_average_filter_prefill(moving_average_filter_t *filter, filter_data_t init_value)
{
    if (filter == NULL || filter->buffer == NULL) {
        return;
    }
    
    for (uint16 i = 0; i < filter->window_size; i++) {
        filter->buffer[i] = init_value;
    }
    
    filter->buffer_index = 0;
    filter->sample_count = filter->window_size;
    filter->sum = init_value * filter->window_size;
    filter->output = init_value;
    
    if (filter->state != FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_READY;
    }
}

/**
 * @brief 初始化去除最值的滑动均值滤波器
 */
exit_code_t moving_average_filter_trim_init(moving_average_filter_trim_t *filter, 
                                            filter_data_t *buffer, 
                                            uint16 window_size,
                                            uint8 remove_count)
{
    if (filter == NULL || buffer == NULL || window_size == 0) {
        return EXIT_INVALID_PARAM;
    }
    
    // 检查参数合理性：去除的数量不能太多
    if (remove_count * 2 >= window_size) {
        return EXIT_INVALID_PARAM;
    }
    
    // 建议窗口大小至少为5
    if (window_size < 5) {
        return EXIT_INVALID_PARAM;
    }
    
    filter->buffer = buffer;
    filter->window_size = window_size;
    filter->buffer_index = 0;
    filter->sample_count = 0;
    filter->remove_count = remove_count;
    filter->output = 0.0f;
    filter->state = FILTER_STATE_INITIALIZED;
    
    // 清空缓冲区
    memset(buffer, 0, sizeof(filter_data_t) * window_size);
    
    return EXIT_OK;
}

/**
 * @brief 去除最值的滑动均值滤波器处理单个数据
 */
filter_data_t moving_average_filter_trim_update(moving_average_filter_trim_t *filter, filter_data_t input)
{
    if (filter == NULL || filter->buffer == NULL || filter->state == FILTER_STATE_ERROR) {
        return 0.0f;
    }
    
    if (filter->state == FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_ERROR;
        return 0.0f;
    }
    
    // 添加新数据到缓冲区
    filter->buffer[filter->buffer_index] = input;
    
    // 更新索引和计数
    filter->buffer_index++;
    if (filter->buffer_index >= filter->window_size) {
        filter->buffer_index = 0;
    }
    
    if (filter->sample_count < filter->window_size) {
        filter->sample_count++;
    }
    
    // 只有当缓冲区有足够数据时才进行滤波计算
    if (filter->sample_count >= filter->window_size) {
        // 创建临时数组用于排序
        filter_data_t temp_buffer[filter->window_size];
        memcpy(temp_buffer, filter->buffer, sizeof(filter_data_t) * filter->window_size);
        
        // 快速排序
        qsort(temp_buffer, filter->window_size, sizeof(filter_data_t), compare_float);
        
        // 计算去除最值后的平均值
        filter_data_t sum = 0.0f;
        uint16 valid_count = filter->window_size - 2 * filter->remove_count;
        
        for (uint16 i = filter->remove_count; i < filter->window_size - filter->remove_count; i++) {
            sum += temp_buffer[i];
        }
        
        filter->output = sum / valid_count;
        filter->state = FILTER_STATE_READY;
    } else {
        // 数据不够时，直接返回当前输入
        filter->output = input;
    }
    
    return filter->output;
}

/**
 * @brief 重置去除最值的滑动均值滤波器
 */
void moving_average_filter_trim_reset(moving_average_filter_trim_t *filter)
{
    if (filter == NULL || filter->buffer == NULL) {
        return;
    }
    
    filter->buffer_index = 0;
    filter->sample_count = 0;
    filter->output = 0.0f;
    
    if (filter->state != FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_INITIALIZED;
    }
    
    // 清空缓冲区
    memset(filter->buffer, 0, sizeof(filter_data_t) * filter->window_size);
}

/**
 * @brief 预填充去除最值的滑动均值滤波器
 */
void moving_average_filter_trim_prefill(moving_average_filter_trim_t *filter, filter_data_t init_value)
{
    if (filter == NULL || filter->buffer == NULL) {
        return;
    }
    
    for (uint16 i = 0; i < filter->window_size; i++) {
        filter->buffer[i] = init_value;
    }
    
    filter->buffer_index = 0;
    filter->sample_count = filter->window_size;
    filter->output = init_value;  // 所有值相同，去除最值后仍然是这个值
    
    if (filter->state != FILTER_STATE_UNINITIALIZED) {
        filter->state = FILTER_STATE_READY;
    }
}

/**
 * @brief 获取简单滑动均值滤波器状态
 */
filter_state_t moving_average_filter_get_state(const moving_average_filter_t *filter)
{
    if (filter == NULL) {
        return FILTER_STATE_ERROR;
    }
    return filter->state;
}

/**
 * @brief 获取去除最值滑动均值滤波器状态
 */
filter_state_t moving_average_filter_trim_get_state(const moving_average_filter_trim_t *filter)
{
    if (filter == NULL) {
        return FILTER_STATE_ERROR;
    }
    return filter->state;
}

/**
 * @brief 获取简单滑动均值滤波器窗口填充百分比
 */
float moving_average_filter_get_fill_ratio(const moving_average_filter_t *filter)
{
    if (filter == NULL || filter->window_size == 0) {
        return 0.0f;
    }
    
    return (float)filter->sample_count / (float)filter->window_size;
}

/**
 * @brief 获取去除最值滑动均值滤波器窗口填充百分比
 */
float moving_average_filter_trim_get_fill_ratio(const moving_average_filter_trim_t *filter)
{
    if (filter == NULL || filter->window_size == 0) {
        return 0.0f;
    }
    
    return (float)filter->sample_count / (float)filter->window_size;
}

