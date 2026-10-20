#include "pid.h"
#include "module/sys/sys_log.h"

#ifdef USE_CMSIS_DSP
#include "arm_math.h"

// CMSIS-DSP PID 实例（仅在 USE_CMSIS_DSP 启用时存在）
// 注意：arm_pid_instance_f32 内部状态与我们的结构体不同，需要映射
#endif

void pid_init(pid_controller_t* pid, float kp, float ki, float kd, float dt, 
              float output_min, float output_max)
{
    if (!pid) {
        sys_log_text(error, "PID: Init failed - NULL pointer");
        return;
    }
    
    sys_log_text(debug, "PID: Initializing controller (kp=%.3f, ki=%.3f, kd=%.3f, dt=%.4f)", kp, ki, kd, dt);
    
    // 设置PID参数
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;
    pid->dt = dt;
    
    // 设置输出限制
    pid->output_min = output_min;
    pid->output_max = output_max;
    
    // 设置默认积分限制
    pid->integral_min = output_min / (ki != 0 ? ki : 1.0f);
    pid->integral_max = output_max / (ki != 0 ? ki : 1.0f);
    
    // 初始化状态变量
    pid->error_prev = 0.0f;
    pid->integral = 0.0f;
    pid->output = 0.0f;
    
    sys_log_text(debug, "PID: Controller initialized (output_range=[%.2f, %.2f])", output_min, output_max);
}

void pid_set_integral_limits(pid_controller_t* pid, float integral_min, float integral_max)
{
    if (!pid) return;
    
    pid->integral_min = integral_min;
    pid->integral_max = integral_max;
}

float pid_calculate(pid_controller_t* pid, float setpoint, float feedback)
{
    if (!pid) return 0.0f;
    if (pid->dt == 0.0f) return 0.0f;  // 防止除零
    
    // 计算误差
    float error = setpoint - feedback;
    
#ifdef USE_CMSIS_DSP
    // 使用 DSP 加速乘法（对于单次 PID 计算提升有限，但代码一致性更好）
    float proportional, integral_term, derivative, derivative_term;
    
    // 比例项
    arm_mult_f32(&pid->kp, &error, &proportional, 1);
    
    // 积分项
    float error_dt = error * pid->dt;
    arm_add_f32(&pid->integral, &error_dt, &pid->integral, 1);
    
    // 积分限幅
    pid->integral = (pid->integral > pid->integral_max) ? pid->integral_max : 
                    (pid->integral < pid->integral_min) ? pid->integral_min : pid->integral;
    
    arm_mult_f32(&pid->ki, &pid->integral, &integral_term, 1);
    
    // 微分项
    float error_diff = error - pid->error_prev;
    derivative = error_diff / pid->dt;
    arm_mult_f32(&pid->kd, &derivative, &derivative_term, 1);
    
    // 计算总输出（使用 DSP 加法）
    float temp;
    arm_add_f32(&proportional, &integral_term, &temp, 1);
    arm_add_f32(&temp, &derivative_term, &pid->output, 1);
    
#else
    // 原始实现（保留）
    // 比例项
    float proportional = pid->kp * error;
    
    // 积分项
    pid->integral += error * pid->dt;
    
    // 积分限幅（防止积分饱和）
    if (pid->integral > pid->integral_max) {
        pid->integral = pid->integral_max;
    } else if (pid->integral < pid->integral_min) {
        pid->integral = pid->integral_min;
    }
    
    float integral_term = pid->ki * pid->integral;
    
    // 微分项
    float derivative = (error - pid->error_prev) / pid->dt;
    float derivative_term = pid->kd * derivative;
    
    // 计算总输出
    pid->output = proportional + integral_term + derivative_term;
#endif
    
    // 输出限幅
    if (pid->output > pid->output_max) {
        pid->output = pid->output_max;
    } else if (pid->output < pid->output_min) {
        pid->output = pid->output_min;
    }
    
    // 更新状态
    pid->error_prev = error;
    
    return pid->output;
}

void pid_update_params(pid_controller_t* pid, float kp, float ki, float kd)
{
    if (!pid) return;
    
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;
    
    // 重新计算积分限制
    if (ki != 0) {
        pid->integral_max = pid->output_max / ki;
        pid->integral_min = pid->output_min / ki;
    }
}

void pid_reset(pid_controller_t* pid)
{
    if (!pid) return;
    
    pid->error_prev = 0.0f;
    pid->integral = 0.0f;
    pid->output = 0.0f;
}

void pid_get_params(pid_controller_t* pid, float* kp, float* ki, float* kd)
{
    if (!pid || !kp || !ki || !kd) return;
    
    *kp = pid->kp;
    *ki = pid->ki;
    *kd = pid->kd;
}

#ifdef USE_CMSIS_DSP
/**
 * @brief 初始化 DSP PID 控制器
 */
void pid_dsp_init(pid_controller_dsp_t* pid, float kp, float ki, float kd,
                  float output_min, float output_max)
{
    if (!pid) return;
    
    // 先写入 PID 参数，再初始化内部系数（否则 init 会基于 0 系数计算 A0/A1/A2）
    pid->instance.Kp = kp;
    pid->instance.Ki = ki;
    pid->instance.Kd = kd;
    arm_pid_init_f32(&pid->instance, 1);  // 1 表示复位 PID 状态
    
    // 设置输出限制
    pid->output_min = output_min;
    pid->output_max = output_max;
    pid->output = 0.0f;
}

/**
 * @brief 使用 DSP 加速计算 PID 输出
 */
float pid_dsp_calculate(pid_controller_dsp_t* pid, float setpoint, float feedback)
{
    if (!pid) return 0.0f;

    // 使用 CMSIS-DSP 硬件加速计算 PID
    float error = setpoint - feedback;
    pid->output = arm_pid_f32(&pid->instance, error);
    
    // 输出限幅
    if (pid->output > pid->output_max) {
        pid->output = pid->output_max;
    } else if (pid->output < pid->output_min) {
        pid->output = pid->output_min;
    }
    
    return pid->output;
}

/**
 * @brief 重置 DSP PID 控制器状态
 */
void pid_dsp_reset(pid_controller_dsp_t* pid)
{
    if (!pid) return;
    
    // 重新初始化（清除积分和微分状态，保留当前 K 值）
    arm_pid_init_f32(&pid->instance, 1);
    pid->output = 0.0f;
}

/**
 * @brief 更新 DSP PID 参数
 */
void pid_dsp_update_params(pid_controller_dsp_t* pid, float kp, float ki, float kd)
{
    if (!pid) return;
    
    pid->instance.Kp = kp;
    pid->instance.Ki = ki;
    pid->instance.Kd = kd;
    // 重新计算内部系数，保持状态（积分/微分）不清零
    arm_pid_init_f32(&pid->instance, 0);
}
#endif // USE_CMSIS_DSP
