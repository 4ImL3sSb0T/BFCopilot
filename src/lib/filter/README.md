# 滤波器库

本文件夹包含了多种数字滤波器实现，适用于智能车控制系统中的信号处理需求。

## 功能模块

### 1. 低通滤波器 (`low_pass_filter.h/.c`)
- **一阶低通滤波器**：简单高效，适用于去除高频噪声
- **二阶低通滤波器**：更好的滤波效果，基于Butterworth设计

#### 主要特点：
- 支持通过截止频率和采样频率自动计算滤波系数
- 支持直接设置滤波系数
- 提供滤波器状态查询功能
- 支持滤波器重置和初始化

#### 使用场景：
- 传感器数据去噪（如编码器速度滤波）
- IMU数据平滑处理
- 电机控制信号滤波

### 2. 滑动均值滤波器 (`moving_average_filter.h/.c`)
- **简单滑动均值**：对窗口内所有数据求平均
- **去除最值滑动均值**：去除最大最小值后求平均，抗干扰能力强

#### 主要特点：
- 使用用户提供的缓冲区，内存可控
- 支持窗口预填充
- 提供窗口填充率查询
- 去除最值版本可有效去除突变噪声

#### 使用场景：
- 传感器原始数据预处理
- 电压、电流监测数据平滑
- 环境传感器数据稳定化

### 3. 通用定义 (`filter_common.h`)
- 统一的数据类型定义
- 滤波器状态枚举
- 错误码集成

## 使用方法

### 低通滤波器示例：

```c
#include "filter.h"

// 创建一阶低通滤波器
low_pass_filter_t speed_filter;

void init_filters(void) {
    // 初始化：截止频率10Hz，采样频率100Hz
    low_pass_filter_init(&speed_filter, 10.0f, 100.0f);
}

void process_encoder_data(void) {
    int16 raw_speed = encoder_get_speed(&encoder_left);
    float filtered_speed = low_pass_filter_update(&speed_filter, (float)raw_speed);
    
    // 使用滤波后的速度数据
    // ...
}
```

### 滑动均值滤波器示例：

```c
#include "filter.h"

#define WINDOW_SIZE 10
filter_data_t sensor_buffer[WINDOW_SIZE];
moving_average_filter_trim_t sensor_filter;

void init_filters(void) {
    // 初始化去除最值的滑动均值滤波器
    moving_average_filter_trim_init(&sensor_filter, sensor_buffer, WINDOW_SIZE, 1);
}

void process_sensor_data(void) {
    float raw_data = read_sensor();
    float filtered_data = moving_average_filter_trim_update(&sensor_filter, raw_data);
    
    // 使用滤波后的数据
    // ...
}
```

## 设计原则

1. **内存安全**：使用用户提供的缓冲区，避免动态内存分配
2. **错误处理**：完善的参数检查和状态管理
3. **性能优化**：算法实现考虑了单片机的计算能力限制
4. **代码风格**：与项目整体代码风格保持一致
5. **可扩展性**：模块化设计，便于添加新的滤波器类型

## 注意事项

1. 使用滑动均值滤波器时，需要确保提供的缓冲区大小与窗口大小匹配
2. 低通滤波器的截止频率应小于采样频率的一半（奈奎斯特频率）
3. 去除最值的滤波器建议窗口大小至少为5
4. 所有滤波器在使用前都需要初始化
5. 可以通过状态查询函数检查滤波器是否正常工作

## 性能参考

- 一阶低通滤波器：约5-10个浮点运算/次
- 二阶低通滤波器：约15-20个浮点运算/次  
- 简单滑动均值：约2-3个浮点运算/次
- 去除最值滑动均值：约N*log(N)复杂度（N为窗口大小）
