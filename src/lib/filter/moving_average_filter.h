#ifndef __MOVING_AVERAGE_FILTER_H__
#define __MOVING_AVERAGE_FILTER_H__

#include "filter_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 简单滑动均值滤波器结构体
 * 
 * 对窗口内的所有数据求平均值
 */
typedef struct {
    filter_data_t *buffer;      // 数据缓冲区
    uint16 window_size;         // 窗口大小
    uint16 buffer_index;        // 当前缓冲区索引
    uint32 sample_count;        // 采样计数
    filter_data_t sum;          // 当前窗口数据和
    filter_data_t output;       // 滤波输出
    filter_state_t state;       // 滤波器状态
} moving_average_filter_t;

/**
 * @brief 去除最大最小值的滑动均值滤波器结构体
 * 
 * 在窗口内去除最大值和最小值后求平均值
 * 可以有效去除突变噪声
 */
typedef struct {
    filter_data_t *buffer;      // 数据缓冲区
    uint16 window_size;         // 窗口大小
    uint16 buffer_index;        // 当前缓冲区索引
    uint32 sample_count;        // 采样计数
    uint8 remove_count;         // 去除的最值个数(每边去除的个数)
    filter_data_t output;       // 滤波输出
    filter_state_t state;       // 滤波器状态
} moving_average_filter_trim_t;

/**
 * @brief 初始化简单滑动均值滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param buffer 数据缓冲区(由用户提供)
 * @param window_size 窗口大小
 * @return exit_code_t 错误码
 */
exit_code_t moving_average_filter_init(moving_average_filter_t *filter, 
                                       filter_data_t *buffer, 
                                       uint16 window_size);

/**
 * @brief 简单滑动均值滤波器处理单个数据
 * 
 * @param filter 滤波器结构体指针
 * @param input 输入数据
 * @return filter_data_t 滤波后的输出
 */
filter_data_t moving_average_filter_update(moving_average_filter_t *filter, filter_data_t input);

/**
 * @brief 重置简单滑动均值滤波器
 * 
 * @param filter 滤波器结构体指针
 */
void moving_average_filter_reset(moving_average_filter_t *filter);

/**
 * @brief 预填充简单滑动均值滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param init_value 初始填充值
 */
void moving_average_filter_prefill(moving_average_filter_t *filter, filter_data_t init_value);

/**
 * @brief 初始化去除最值的滑动均值滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param buffer 数据缓冲区(由用户提供)
 * @param window_size 窗口大小(建议>=5)
 * @param remove_count 每边去除的最值个数(通常为1)
 * @return exit_code_t 错误码
 */
exit_code_t moving_average_filter_trim_init(moving_average_filter_trim_t *filter, 
                                            filter_data_t *buffer, 
                                            uint16 window_size,
                                            uint8 remove_count);

/**
 * @brief 去除最值的滑动均值滤波器处理单个数据
 * 
 * @param filter 滤波器结构体指针
 * @param input 输入数据
 * @return filter_data_t 滤波后的输出
 */
filter_data_t moving_average_filter_trim_update(moving_average_filter_trim_t *filter, filter_data_t input);

/**
 * @brief 重置去除最值的滑动均值滤波器
 * 
 * @param filter 滤波器结构体指针
 */
void moving_average_filter_trim_reset(moving_average_filter_trim_t *filter);

/**
 * @brief 预填充去除最值的滑动均值滤波器
 * 
 * @param filter 滤波器结构体指针
 * @param init_value 初始填充值
 */
void moving_average_filter_trim_prefill(moving_average_filter_trim_t *filter, filter_data_t init_value);

/**
 * @brief 获取简单滑动均值滤波器状态
 * 
 * @param filter 滤波器结构体指针
 * @return filter_state_t 滤波器状态
 */
filter_state_t moving_average_filter_get_state(const moving_average_filter_t *filter);

/**
 * @brief 获取去除最值滑动均值滤波器状态
 * 
 * @param filter 滤波器结构体指针
 * @return filter_state_t 滤波器状态
 */
filter_state_t moving_average_filter_trim_get_state(const moving_average_filter_trim_t *filter);

/**
 * @brief 获取简单滑动均值滤波器窗口填充百分比
 * 
 * @param filter 滤波器结构体指针
 * @return float 填充百分比 (0.0 - 1.0)
 */
float moving_average_filter_get_fill_ratio(const moving_average_filter_t *filter);

/**
 * @brief 获取去除最值滑动均值滤波器窗口填充百分比
 * 
 * @param filter 滤波器结构体指针
 * @return float 填充百分比 (0.0 - 1.0)
 */
float moving_average_filter_trim_get_fill_ratio(const moving_average_filter_trim_t *filter);

#ifdef __cplusplus
}
#endif

#endif // !__MOVING_AVERAGE_FILTER_H__