#include "fuzzy_control.h"
#include <math.h>
#include <string.h>

// 默认模糊规则表：根据error和delta_error确定输出
// 行：error (NB, NM, NS, ZO, PS, PM, PB)
// 列：delta_error (NB, NM, NS, ZO, PS, PM, PB)
const int default_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT] = {
    // delta_error: NB  NM  NS  ZO  PS  PM  PB
    /* NB */      { PB, PB, PM, PM, PS, ZO, ZO },
    /* NM */      { PB, PM, PM, PS, PS, ZO, NS },
    /* NS */      { PM, PM, PS, PS, ZO, NS, NS },
    /* ZO */      { PM, PS, PS, ZO, NS, NS, NM },
    /* PS */      { PS, PS, ZO, NS, NS, NM, NM },
    /* PM */      { PS, ZO, NS, NM, NM, NM, NB },
    /* PB */      { ZO, ZO, NM, NM, NM, NB, NB }
};

void fuzzy_control_init(fuzzy_control_t* ctrl, value_range_t error_range, 
                       value_range_t delta_error_range, value_range_t output_range,
                       const int rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT])
{
    if (!ctrl) return;
    
    // 初始化状态变量
    ctrl->error = 0.0f;
    ctrl->delta_error = 0.0f;
    
    // 设置值范围
    ctrl->error_range = error_range;
    ctrl->delta_error_range = delta_error_range;
    ctrl->output_range = output_range;
    
    // 复制规则表
    if (rule_table) {
        memcpy(ctrl->rule, rule_table, sizeof(ctrl->rule));
    } else {
        memcpy(ctrl->rule, default_rule_table, sizeof(default_rule_table));
    }
    
    // 初始化隶属度
    memset(ctrl->membership, 0, sizeof(ctrl->membership));
}

float fuzzy_control_quantization(float max, float min, float value)
{
    if (max == min) return 0.0f;  // 防止除零，返回ZO
    if (value > max) value = max;
    if (value < min) value = min;
    
    // 先映射到[-3, 3]范围
    float fuzzy_value = (value - min) / (max - min) * 6.0f - 3.0f; // 映射到[-3, 3]
    return fuzzy_value;
}

float fuzzy_control_dequantization(float max, float min, float value)
{
    return (value + 3.0f) / 6.0f * (max - min) + min; // 从[-3, 3]映射回实际范围
}

void fuzzy_control_membership(float value, float* membership)
{
    // 初始化隶属度
    for (int i = 0; i < FUZZY_SET_COUNT; i++) {
        membership[i] = 0.0f;
    }
    
    // 限制value在[-3, 3]范围内
    if (value < -3.0f) value = -3.0f;
    if (value > 3.0f) value = 3.0f;
    
    // 将模糊值[-3, 3]转换为数组索引[0, 6]
    float index_value = value + 3.0f;
    
    // 计算三角形隶属度函数
    int left_index = (int)index_value;
    int right_index = left_index + 1;
    
    if (right_index >= FUZZY_SET_COUNT) {
        membership[FUZZY_SET_COUNT - 1] = 1.0f;
    } else {
        float right_weight = index_value - left_index;
        float left_weight = 1.0f - right_weight;
        
        membership[left_index] = left_weight;
        membership[right_index] = right_weight;
    }
}

float fuzzy_control_inference(fuzzy_control_t* ctrl, float* error_membership, float* delta_error_membership)
{
    float output_membership[FUZZY_SET_COUNT] = {0};
    float total_weight = 0;
    float weighted_sum = 0;
    
    // 模糊推理
    for (int i = 0; i < FUZZY_SET_COUNT; i++) {
        for (int j = 0; j < FUZZY_SET_COUNT; j++) {
            if (error_membership[i] > 0 && delta_error_membership[j] > 0) {
                float rule_strength = fminf(error_membership[i], delta_error_membership[j]);
                int output_set = ctrl->rule[i][j];
                
                // 将模糊集枚举值(-3~3)转换为数组索引(0~6)
                int output_index = output_set - NB;
                if (output_index >= 0 && output_index < FUZZY_SET_COUNT) {
                    if (rule_strength > output_membership[output_index]) {
                        output_membership[output_index] = rule_strength;
                    }
                }
            }
        }
    }
    
    // 重心法去模糊化
    for (int i = 0; i < FUZZY_SET_COUNT; i++) {
        // 将数组索引转换为对应的模糊值 (i-3 对应 -3到3)
        weighted_sum += output_membership[i] * (i - 3);
        total_weight += output_membership[i];
    }
    
    if (total_weight > 0) {
        return weighted_sum / total_weight;
    }
    
    return 0.0f;
}

float fuzzy_control_calculate(fuzzy_control_t* ctrl, float error, float delta_error)
{
    if (!ctrl) return 0.0f;
    
    // 更新状态
    ctrl->error = error;
    ctrl->delta_error = delta_error;
    
    // 量化输入到模糊值域[-3, 3]
    float quantized_error = fuzzy_control_quantization(ctrl->error_range.max, ctrl->error_range.min, error);
    float quantized_delta_error = fuzzy_control_quantization(ctrl->delta_error_range.max, ctrl->delta_error_range.min, delta_error);
    
    // 计算隶属度
    float error_membership[FUZZY_SET_COUNT];
    float delta_error_membership[FUZZY_SET_COUNT];
    
    fuzzy_control_membership(quantized_error, error_membership);
    fuzzy_control_membership(quantized_delta_error, delta_error_membership);
    
    // 模糊推理
    float output_value = fuzzy_control_inference(ctrl, error_membership, delta_error_membership);
    
    // 去量化输出
    return fuzzy_control_dequantization(ctrl->output_range.max, ctrl->output_range.min, output_value);
}

void fuzzy_control_update_rules(fuzzy_control_t* ctrl, const int rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT])
{
    if (!ctrl || !rule_table) return;
    
    memcpy(ctrl->rule, rule_table, sizeof(ctrl->rule));
}