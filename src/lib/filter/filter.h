#ifndef __FILTER_H__
#define __FILTER_H__

/**
 * @file filter.h
 * @brief 滤波器库主头文件
 * 
 * 本库提供了多种数字滤波器实现，适用于嵌入式系统中的信号处理需求。
 * 
 * 主要功能：
 * - 一阶和二阶低通滤波器
 * - 简单滑动均值滤波器  
 * - 去除最大最小值的滑动均值滤波器
 * 
 * 特点：
 * - 使用静态内存分配，适合单片机环境
 * - 提供完整的错误处理机制
 * - 支持滤波器状态查询
 * - 代码风格与项目保持一致
 * 
 * 使用示例：
 * @code
 * // 低通滤波器使用示例
 * low_pass_filter_t lpf;
 * low_pass_filter_init(&lpf, 10.0f, 100.0f); // 截止频率10Hz，采样频率100Hz
 * 
 * float filtered_value = low_pass_filter_update(&lpf, sensor_data);
 * 
 * // 滑动均值滤波器使用示例
 * #define WINDOW_SIZE 10
 * filter_data_t buffer[WINDOW_SIZE];
 * moving_average_filter_t maf;
 * moving_average_filter_init(&maf, buffer, WINDOW_SIZE);
 * 
 * float averaged_value = moving_average_filter_update(&maf, sensor_data);
 * @endcode
 * 
 * @author AI Assistant
 * @date 2025-09-28
 * @version 1.0
 */

#include "filter_common.h"
#include "low_pass_filter.h"
#include "moving_average_filter.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 滤波器库版本信息
 */
#define FILTER_LIB_VERSION_MAJOR    1
#define FILTER_LIB_VERSION_MINOR    0
#define FILTER_LIB_VERSION_PATCH    0

/**
 * @brief 获取滤波器库版本字符串
 * 
 * @return const char* 版本字符串
 */
const char* filter_get_version(void);

/**
 * @brief 滤波器库初始化
 * 
 * 执行库级别的初始化操作（如果需要）
 * 
 * @return exit_code_t 错误码
 */
exit_code_t filter_lib_init(void);

#ifdef __cplusplus
}
#endif

#endif // !__FILTER_H__