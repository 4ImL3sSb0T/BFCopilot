# 模糊PID控制器重构文档

## 概述

本项目将原本耦合在一起的模糊PID控制器重构为三个独立的模块：

1. **传统PID控制器** (`pid.h` / `pid.c`)
2. **模糊控制器** (`fuzzy_control.h` / `fuzzy_control.c`)
3. **模糊PID控制器** (`fuzzy_pid.h` / `fuzzy_pid.c`)

**重要特性**：所有模块都使用**静态内存分配**，完全适合单片机环境，无需担心动态内存分配的问题。

## 架构设计

```
┌─────────────────┐    ┌──────────────────┐
│   fuzzy_pid     │    │   fuzzy_control  │
│  (模糊PID控制器) │◄───┤   (模糊控制器)    │
└─────────┬───────┘    └──────────────────┘
          │
          ▼
┌─────────────────┐
│      pid        │
│  (传统PID控制器) │
└─────────────────┘
```

## 模块说明

### 1. 传统PID控制器 (`pid.h` / `pid.c`)

**职责**：实现标准的PID控制算法

**特性**：
- 基础的比例、积分、微分计算
- 输出限幅功能
- 积分饱和保护
- 参数动态调整接口

**主要函数**：
- `pid_init()` - 初始化PID控制器
- `pid_calculate()` - 计算PID输出
- `pid_update_params()` - 更新PID参数
- `pid_reset()` - 重置控制器状态

### 2. 模糊控制器 (`fuzzy_control.h` / `fuzzy_control.c`)

**职责**：实现模糊逻辑推理

**特性**：
- 7个模糊集合 (NB, NM, NS, ZO, PS, PM, PB)
- 三角形隶属度函数
- 可配置的模糊规则表
- 重心法去模糊化

**主要函数**：
- `fuzzy_control_init()` - 初始化模糊控制器
- `fuzzy_control_calculate()` - 计算模糊控制输出
- `fuzzy_control_update_rules()` - 更新模糊规则
- 各种内部函数：量化、隶属度计算、推理等

### 3. 模糊PID控制器 (`fuzzy_pid.h` / `fuzzy_pid.c`)

**职责**：整合传统PID和模糊控制，实现参数自适应

**特性**：
- 直接嵌入传统PID控制器实例（非指针）
- 直接嵌入三个模糊控制器实例（分别用于调整Kp、Ki、Kd）
- 支持选择性启用/禁用参数调整
- 使用静态内存分配，适合单片机环境
- 无需手动内存管理

**主要函数**：
- `fuzzy_pid_init()` - 初始化模糊PID控制器
- `fuzzy_pid_calculate()` - 计算自适应PID输出
- `fuzzy_pid_set_rules()` - 设置参数调整规则
- `fuzzy_pid_enable_adaptive()` - 启用/禁用参数调整
- `fuzzy_pid_reset()` - 重置控制器状态

## 使用示例

```c
#include "fuzzy_pid/fuzzy_pid.h"

// 创建模糊PID控制器（静态分配）
fuzzy_pid_t controller;

// 定义参数范围
value_range_t kp_range = {0.5f, 2.0f};
value_range_t ki_range = {0.01f, 0.5f};
value_range_t kd_range = {0.01f, 0.2f};
value_range_t error_range = {-100.0f, 100.0f};
value_range_t delta_error_range = {-50.0f, 50.0f};

// 初始化
fuzzy_pid_init(&controller, 
              1.0f, 0.1f, 0.05f,           // 初始PID参数
              kp_range, ki_range, kd_range, // 参数调整范围
              0.01f, -100.0f, 100.0f,       // 采样周期和输出范围
              error_range, delta_error_range); // 误差范围

// 控制循环
for (int i = 0; i < 1000; i++) {
    float output = fuzzy_pid_calculate(&controller, setpoint, feedback);
    // 应用控制输出...
}

// 使用静态内存分配，无需手动清理资源
// 当变量离开作用域时，内存会自动释放
```

## 优势

1. **模块化设计**：每个模块职责单一，易于测试和维护
2. **可重用性**：传统PID和模糊控制器可以独立使用
3. **灵活配置**：支持自定义规则表和参数范围
4. **静态内存分配**：完全适合单片机环境，无动态内存分配
5. **内存安全**：无需手动内存管理，避免内存泄漏
6. **向后兼容**：保持原有的API使用方式
7. **单片机友好**：无内存碎片问题，栈空间使用可预测

## 文件结构

```
main/fuzzy_pid/
├── pid.h                    # 传统PID控制器头文件
├── pid.c                    # 传统PID控制器实现
├── fuzzy_control.h          # 模糊控制器头文件
├── fuzzy_control.c          # 模糊控制器实现
├── fuzzy_pid.h              # 模糊PID控制器头文件
├── fuzzy_pid.c              # 模糊PID控制器实现
├── fuzzy_pid_example.c      # 使用示例
└── README.md                # 本文档
```

## 编译配置

项目使用ESP-IDF构建系统，CMakeLists.txt会自动收集所有的.c文件进行编译，无需额外配置。

## 单片机适配说明

### 内存使用

所有控制器都使用静态内存分配：
- `pid_controller_t`: 约 36 字节
- `fuzzy_control_t`: 约 232 字节（包含7x7规则表和隶属度数组）
- `fuzzy_pid_t`: 约 736 字节（包含1个PID + 3个模糊控制器）

### 栈空间使用

函数调用过程中的临时变量使用栈空间：
- `fuzzy_pid_calculate()`: 约 100 字节临时变量
- `fuzzy_control_calculate()`: 约 80 字节临时变量

### 适用性

✅ **适合的场景**：
- ESP32、STM32等有足够RAM的单片机
- 对内存使用可预测性要求高的应用
- 不希望使用动态内存分配的嵌入式系统

⚠️ **注意事项**：
- 对于RAM非常有限的单片机（如某些8位MCU），可考虑只使用传统PID控制器
- 可根据实际需求选择性禁用某些模糊控制器以节省内存