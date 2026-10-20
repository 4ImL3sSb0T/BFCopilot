#include "fuzzy_pid.h"
#include <math.h>
#include <string.h>

// 专家经验规则表
int default_kp_rule_table[7][7] =  {{PB,PB,PM,PM,PS,ZO,ZO},     //kp规则表
                                    {PB,PB,PM,PS,PS,ZO,NS},
                                    {PM,PM,PM,PS,ZO,NS,NS},
                                    {PM,PM,PS,ZO,NS,NM,NM},
                                    {PS,PS,ZO,NS,NS,NM,NM},
                                    {PS,ZO,NS,NM,NM,NM,NB},
                                    {ZO,ZO,NM,NM,NM,NB,NB}};

int default_ki_rule_table[7][7] =  {{NB,NB,NM,NM,NS,ZO,ZO},     //ki规则表
                                    {NB,NB,NM,NS,NS,ZO,ZO},
                                    {NB,NM,NS,NS,ZO,PS,PS},
                                    {NM,NM,NS,ZO,PS,PM,PM},
                                    {NM,NS,ZO,PS,PS,PM,PB},
                                    {ZO,ZO,PS,PS,PM,PB,PB},
                                    {ZO,ZO,PS,PM,PM,PB,PB}};

int default_kd_rule_table[7][7] =  {{PS,NS,NB,NB,NB,NM,PS},    //kd规则表
                                    {PS,NS,NB,NM,NM,NS,ZO},
                                    {ZO,NS,NM,NM,NS,NS,ZO},
                                    {ZO,NS,NS,NS,NS,NS,ZO},
                                    {ZO,ZO,ZO,ZO,ZO,ZO,ZO},
                                    {PB,NS,PS,PS,PS,PS,PB},
                                    {PB,PM,PM,PM,PS,PS,PB}};

void fuzzy_pid_init(fuzzy_pid_t* pid, 
                   float initial_kp, float initial_ki, float initial_kd,
                   value_range_t kp_range, value_range_t ki_range, value_range_t kd_range,
                   float dt, float output_min, float output_max,
                   value_range_t error_range, value_range_t delta_error_range)
{
    if (!pid) return;
    
    // 初始化传统PID控制器（直接使用结构体成员，无需malloc）
    pid_init(&pid->pid_ctrl, initial_kp, initial_ki, initial_kd, dt, output_min, output_max);
    
    // 保存参数范围
    pid->kp_range = kp_range;
    pid->ki_range = ki_range;
    pid->kd_range = kd_range;
    
    // 初始化模糊控制器（直接使用结构体成员，无需malloc）
    value_range_t adjustment_range = {-1.0f, 1.0f}; // 参数调整范围-1到1
    fuzzy_control_init(&pid->kp_fuzzy, error_range, delta_error_range, adjustment_range, NULL);
    fuzzy_control_init(&pid->ki_fuzzy, error_range, delta_error_range, adjustment_range, NULL);
    fuzzy_control_init(&pid->kd_fuzzy, error_range, delta_error_range, adjustment_range, NULL);
    
    // 默认启用所有参数的自适应调整
    pid->enable_kp_adjust = 1;
    pid->enable_ki_adjust = 1;
    pid->enable_kd_adjust = 1;
}

void fuzzy_pid_set_rules(fuzzy_pid_t* pid,
                        const int kp_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT],
                        const int ki_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT],
                        const int kd_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT])
{
    if (!pid) return;
    
    if (kp_rule_table) {
        fuzzy_control_update_rules(&pid->kp_fuzzy, kp_rule_table);
    }
    
    if (ki_rule_table) {
        fuzzy_control_update_rules(&pid->ki_fuzzy, ki_rule_table);
    }
    
    if (kd_rule_table) {
        fuzzy_control_update_rules(&pid->kd_fuzzy, kd_rule_table);
    }
}

void fuzzy_pid_enable_adaptive(fuzzy_pid_t* pid, int enable_kp, int enable_ki, int enable_kd)
{
    if (!pid) return;
    
    pid->enable_kp_adjust = enable_kp;
    pid->enable_ki_adjust = enable_ki;
    pid->enable_kd_adjust = enable_kd;
}

float fuzzy_pid_calculate(fuzzy_pid_t* pid, float setpoint, float feedback, float dt)
{
    if (!pid) return 0.0f;
    
    // 计算误差和误差变化量
    float error = setpoint - feedback;
    float delta_error = error - pid->pid_ctrl.error_prev;
    
    // 获取当前PID参数
    float current_kp, current_ki, current_kd;
    pid_get_params(&pid->pid_ctrl, &current_kp, &current_ki, &current_kd);
    
    // 使用模糊控制调整PID参数
    float new_kp = current_kp;
    float new_ki = current_ki;
    float new_kd = current_kd;
    
    if (pid->enable_kp_adjust) {
        float kp_adjustment = fuzzy_control_calculate(&pid->kp_fuzzy, error, delta_error);
        new_kp = current_kp + kp_adjustment * (pid->kp_range.max - pid->kp_range.min) * dt;
        
        // 限制Kp范围
        if (new_kp > pid->kp_range.max) new_kp = pid->kp_range.max;
        if (new_kp < pid->kp_range.min) new_kp = pid->kp_range.min;
    }
    
    if (pid->enable_ki_adjust) {
        float ki_adjustment = fuzzy_control_calculate(&pid->ki_fuzzy, error, delta_error);
        new_ki = current_ki + ki_adjustment * (pid->ki_range.max - pid->ki_range.min) * dt;
        
        // 限制Ki范围
        if (new_ki > pid->ki_range.max) new_ki = pid->ki_range.max;
        if (new_ki < pid->ki_range.min) new_ki = pid->ki_range.min;
    }
    
    if (pid->enable_kd_adjust) {
        float kd_adjustment = fuzzy_control_calculate(&pid->kd_fuzzy, error, delta_error);
        new_kd = current_kd + kd_adjustment * (pid->kd_range.max - pid->kd_range.min) * dt;
        
        // 限制Kd范围
        if (new_kd > pid->kd_range.max) new_kd = pid->kd_range.max;
        if (new_kd < pid->kd_range.min) new_kd = pid->kd_range.min;
    }
    
    // 更新PID参数
    pid_update_params(&pid->pid_ctrl, new_kp, new_ki, new_kd);
    
    // 计算PID输出
    return pid_calculate(&pid->pid_ctrl, setpoint, feedback);
}

void fuzzy_pid_get_params(fuzzy_pid_t* pid, float* kp, float* ki, float* kd)
{
    if (!pid) return;
    
    pid_get_params(&pid->pid_ctrl, kp, ki, kd);
}

void fuzzy_pid_reset(fuzzy_pid_t* pid)
{
    if (!pid) return;
    
    pid_reset(&pid->pid_ctrl);
}