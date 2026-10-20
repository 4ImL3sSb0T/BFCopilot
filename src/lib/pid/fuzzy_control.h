#ifndef _FUZZY_CONTROL_H_
#define _FUZZY_CONTROL_H_

#ifdef __cplusplus
extern "C" {
#endif

// 模糊集合数量
#define FUZZY_SET_COUNT 7

// 模糊集合定义：NB, NM, NS, ZO, PS, PM, PB
typedef enum {
    NB = -3, // Negative Big
    NM = -2, // Negative Medium
    NS = -1, // Negative Small
    ZO = 0, // Zero
    PS = 1, // Positive Small
    PM = 2, // Positive Medium
    PB = 3  // Positive Big
} fuzzy_set_e;

// 值范围结构体
typedef struct value_range_t
{
    float min;
    float max;
} value_range_t;

// 模糊控制器结构体
typedef struct fuzzy_control_t
{
    float error;                    // 当前误差
    float delta_error;              // 误差变化量

    value_range_t error_range;      // 误差值范围
    value_range_t delta_error_range;// 误差变化量范围
    value_range_t output_range;     // 输出值范围

    // 模糊规则表 [error][delta_error]
    int rule[FUZZY_SET_COUNT][FUZZY_SET_COUNT];
    
    // 隶属度函数值
    float membership[FUZZY_SET_COUNT];
} fuzzy_control_t;

// 默认模糊规则表：根据error和delta_error确定输出
// 行：error (NB, NM, NS, ZO, PS, PM, PB)
// 列：delta_error (NB, NM, NS, ZO, PS, PM, PB)
extern const int default_rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT];

/**
 * @brief 初始化模糊控制器
 * 
 * @param ctrl 模糊控制器指针
 * @param error_range 误差值范围
 * @param delta_error_range 误差变化量范围
 * @param output_range 输出值范围
 * @param rule_table 模糊规则表，如果为NULL则使用默认规则表
 */
void fuzzy_control_init(fuzzy_control_t* ctrl, value_range_t error_range, 
                       value_range_t delta_error_range, value_range_t output_range,
                       const int rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT]);

/**
 * @brief 计算模糊控制输出
 * 
 * @param ctrl 模糊控制器指针
 * @param error 当前误差值
 * @param delta_error 误差变化量
 * @return float 模糊控制输出值
 */
float fuzzy_control_calculate(fuzzy_control_t* ctrl, float error, float delta_error);

/**
 * @brief 量化函数：将实际值映射到模糊域
 * 
 * @param max 量化范围最大值
 * @param min 量化范围最小值  
 * @param value 待量化的实际值
 * @return float 量化后的数组索引值（0-6，对应模糊值-3到3）
 */
float fuzzy_control_quantization(float max, float min, float value);

/**
 * @brief 反量化函数：将模糊域值映射回实际值
 * 
 * @param max 反量化范围最大值
 * @param min 反量化范围最小值
 * @param value 待反量化的数组索引值（0-6，对应模糊值-3到3）
 * @return float 反量化后的实际值
 */
float fuzzy_control_dequantization(float max, float min, float value);

/**
 * @brief 计算隶属度函数值
 * 
 * @param value 输入的模糊值（-3到3，对应NB到PB）
 * @param membership 隶属度数组指针，存储7个模糊集合的隶属度值
 */
void fuzzy_control_membership(float value, float* membership);

/**
 * @brief 模糊推理计算
 * 
 * @param ctrl 模糊控制器指针
 * @param error_membership 误差的隶属度数组
 * @param delta_error_membership 误差变化量的隶属度数组
 * @return float 推理结果（数组索引值0-6，对应模糊值-3到3）
 */
float fuzzy_control_inference(fuzzy_control_t* ctrl, float* error_membership, float* delta_error_membership);

/**
 * @brief 更新模糊规则表
 * 
 * @param ctrl 模糊控制器指针
 * @param rule_table 新的模糊规则表
 */
void fuzzy_control_update_rules(fuzzy_control_t* ctrl, const int rule_table[FUZZY_SET_COUNT][FUZZY_SET_COUNT]);

#ifdef __cplusplus
}
#endif

#endif // _FUZZY_CONTROL_H_