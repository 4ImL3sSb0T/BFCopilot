#ifndef __FILTER_COMMON_H__
#define __FILTER_COMMON_H__

#include "zf_common_headfile.h"
#include "common/tools/common_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 滤波器通用类型定义
 */
typedef float filter_data_t;

/**
 * @brief 滤波器状态枚举
 */
typedef enum {
    FILTER_STATE_UNINITIALIZED = 0,    // 未初始化
    FILTER_STATE_INITIALIZED,          // 已初始化
    FILTER_STATE_READY,                // 准备就绪
    FILTER_STATE_ERROR                 // 错误状态
} filter_state_t;

#ifdef __cplusplus
}
#endif

#endif // !__FILTER_COMMON_H__