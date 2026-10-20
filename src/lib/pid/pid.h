#ifndef __PID_H__
#define __PID_H__

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 传统PID控制器结构体
 */
typedef struct pid_controller_t
{
    // PID参数
    float kp;           // 比例系数
    float ki;           // 积分系数  
    float kd;           // 微分系数
    
    // PID状态变量
    float error_prev;   // 上一次误差
    float integral;     // 积分累积值
    float output;       // 控制器输出
    
    // 输出限制
    float output_max;   // 输出最大值
    float output_min;   // 输出最小值
    
    // 积分限制（防止积分饱和）
    float integral_max; // 积分最大值
    float integral_min; // 积分最小值
    
    // 采样时间
    float dt;           // 采样周期
} pid_controller_t;

#ifdef USE_CMSIS_DSP
/**
 * @brief DSP 加速的 PID 控制器结构体
 * 
 * 使用 CMSIS-DSP 的 arm_pid_f32 实现
 * 提供硬件加速的 PID 计算
 */
typedef struct {
    arm_pid_instance_f32 instance;  // DSP PID 实例
    float output;                    // 控制器输出
    float output_max;                // 输出最大值
    float output_min;                // 输出最小值
} pid_controller_dsp_t;
#endif

/**
 * @brief 初始化PID控制器
 * 
 * @param pid PID控制器指针
 * @param kp 比例系数
 * @param ki 积分系数
 * @param kd 微分系数
 * @param dt 采样周期
 * @param output_min 输出最小值
 * @param output_max 输出最大值
 */
void pid_init(pid_controller_t* pid, float kp, float ki, float kd, float dt, 
              float output_min, float output_max);

/**
 * @brief 设置积分限制
 * 
 * @param pid PID控制器指针
 * @param integral_min 积分最小值
 * @param integral_max 积分最大值
 */
void pid_set_integral_limits(pid_controller_t* pid, float integral_min, float integral_max);

/**
 * @brief 计算PID控制输出
 * 
 * @param pid PID控制器指针
 * @param setpoint 设定值
 * @param feedback 反馈值
 * @return float PID控制输出
 */
float pid_calculate(pid_controller_t* pid, float setpoint, float feedback);

/**
 * @brief 更新PID参数
 * 
 * @param pid PID控制器指针
 * @param kp 新的比例系数
 * @param ki 新的积分系数
 * @param kd 新的微分系数
 */
void pid_update_params(pid_controller_t* pid, float kp, float ki, float kd);

/**
 * @brief 重置PID控制器状态
 * 
 * @param pid PID控制器指针
 */
void pid_reset(pid_controller_t* pid);

/**
 * @brief 获取当前PID参数
 * 
 * @param pid PID控制器指针
 * @param kp 返回比例系数
 * @param ki 返回积分系数
 * @param kd 返回微分系数
 */
void pid_get_params(pid_controller_t* pid, float* kp, float* ki, float* kd);

#ifdef USE_CMSIS_DSP
/**
 * @brief 初始化 DSP PID 控制器
 * 
 * @param pid DSP PID 控制器指针
 * @param kp 比例系数
 * @param ki 积分系数
 * @param kd 微分系数
 * @param output_min 输出最小值
 * @param output_max 输出最大值
 */
void pid_dsp_init(pid_controller_dsp_t* pid, float kp, float ki, float kd,
                  float output_min, float output_max);

/**
 * @brief 使用 DSP 加速计算 PID 输出
 * 
 * @param pid DSP PID 控制器指针
 * @param error 误差值（设定值 - 反馈值）
 * @return float PID 控制输出
 */
float pid_dsp_calculate(pid_controller_dsp_t* pid, float setpoint, float feedback);

/**
 * @brief 重置 DSP PID 控制器状态
 * 
 * @param pid DSP PID 控制器指针
 */
void pid_dsp_reset(pid_controller_dsp_t* pid);

/**
 * @brief 更新 DSP PID 参数
 * 
 * @param pid DSP PID 控制器指针
 * @param kp 新的比例系数
 * @param ki 新的积分系数
 * @param kd 新的微分系数
 */
void pid_dsp_update_params(pid_controller_dsp_t* pid, float kp, float ki, float kd);
#endif // USE_CMSIS_DSP

#ifdef __cplusplus
}
#endif

#endif // __PID_H__
