#ifndef __FUZZY_PID_H__
#define __FUZZY_PID_H__

#include "pid.h"
#include "fuzzy_control.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int default_kp_rule_table[7][7];
extern int default_ki_rule_table[7][7];
extern int default_kd_rule_table[7][7];

/**
 * @brief 模糊PID控制器结构体
 * 
 * 该结构体整合了传统PID控制器和模糊控制器，
 * 用于实现参数自适应的模糊PID控制
 * 使用静态内存分配，适合单片机环境
 */
typedef struct fuzzy_pid_t
{
    // 传统PID控制器（直接嵌入，非指针）
    pid_controller_t pid_ctrl;
    
    // PID参数调整范围
    value_range_t kp_range;   
    value_range_t ki_range;
    value_range_t kd_range;

    // 模糊控制器（用于参数调整，直接嵌入，非指针）
    fuzzy_control_t kp_fuzzy;   // Kp参数调整的模糊控制器
    fuzzy_control_t ki_fuzzy;   // Ki参数调整的模糊控制器  
    fuzzy_control_t kd_fuzzy;   // Kd参数调整的模糊控制器
    
    // 参数调整方式配置
    int enable_kp_adjust;   // 是否启用Kp自适应调整
    int enable_ki_adjust;   // 是否启用Ki自适应调整
    int enable_kd_adjust;   // 是否启用Kd自适应调整
} fuzzy_pid_t;

/**
 * @brief 初始化模糊PID控制器
 * 
 * @param pid 模糊PID控制器指针
 * @param initial_kp 初始Kp值
 * @param initial_ki 初始Ki值
 * @param initial_kd 初始Kd值
 * @param kp_range Kp参数调整范围
 * @param ki_range Ki参数调整范围
 * @param kd_range Kd参数调整范围
 * @param dt PID控制器采样周期
 * @param output_min PID输出最小值
 * @param output_max PID输出最大值
 * @param error_range 误差值范围
 * @param delta_error_range 误差变化量范围
 */
void fuzzy_pid_init(fuzzy_pid_t* pid, 
                   float initial_kp, float initial_ki, float initial_kd,
                   value_range_t kp_range, value_range_t ki_range, value_range_t kd_range,
                   float dt, float output_min, float output_max,
                   value_range_t error_range, value_range_t delta_error_range);

/**
 * @brief 设置模糊PID的参数调整规则
 * 
 * @param pid 模糊PID控制器指针
 * @param kp_rule_table Kp调整规则表，如果为NULL则使用默认规则
 * @param ki_rule_table Ki调整规则表，如果为NULL则使用默认规则
 * @param kd_rule_table Kd调整规则表，如果为NULL则使用默认规则
 */
void fuzzy_pid_set_rules(fuzzy_pid_t* pid,
                        const int kp_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT],
                        const int ki_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT],
                        const int kd_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT]);

/**
 * @brief 启用或禁用参数自适应调整
 * 
 * @param pid 模糊PID控制器指针
 * @param enable_kp 是否启用Kp自适应调整
 * @param enable_ki 是否启用Ki自适应调整
 * @param enable_kd 是否启用Kd自适应调整
 */
void fuzzy_pid_enable_adaptive(fuzzy_pid_t* pid, int enable_kp, int enable_ki, int enable_kd);

/**
 * @brief 计算模糊PID控制输出
 * 
 * @param pid 模糊PID控制器指针
 * @param setpoint 设定值
 * @param feedback 反馈值
 * @return float PID控制输出
 */
float fuzzy_pid_calculate(fuzzy_pid_t* pid, float setpoint, float feedback, float dt);

/**
 * @brief 获取当前PID参数
 * 
 * @param pid 模糊PID控制器指针
 * @param kp 返回当前Kp值
 * @param ki 返回当前Ki值
 * @param kd 返回当前Kd值
 */
void fuzzy_pid_get_params(fuzzy_pid_t* pid, float* kp, float* ki, float* kd);

/**
 * @brief 重置模糊PID控制器状态
 * 
 * @param pid 模糊PID控制器指针
 */
void fuzzy_pid_reset(fuzzy_pid_t* pid);

#ifdef __cplusplus
}
#endif

#endif // __FUZZY_PID_H__