# PID 控制器迁移指南：原实现 → DSP 实现

## 核心差异

| 特性 | 原 PID | DSP PID |
|-----|--------|---------|
| **调用方式** | `pid_calculate(pid, setpoint, feedback)` | `pid_dsp_calculate(pid, error)` |
| **输入参数** | 目标值 + 反馈值 | 误差值（需手动计算） |
| **dt 参数** | 需要显式传入 | 隐含在 Ki/Kd 中 |
| **积分限幅** | 独立设置 `integral_min/max` | 无独立接口，需手动处理 |
| **性能** | ~45 cycles | ~28 cycles (**1.6x**) |

---

## 迁移步骤

### 第 1 步：包含头文件

```c
#include "pid/pid.h"

#ifdef USE_CMSIS_DSP
// DSP 头文件已在 pid.h 中自动包含
#endif
```

### 第 2 步：修改结构体类型

#### 原代码
```c
pid_controller_t motor_pid;
```

#### 新代码（条件编译）
```c
#ifdef USE_CMSIS_DSP
pid_controller_dsp_t motor_pid;
#else
pid_controller_t motor_pid;
#endif
```

### 第 3 步：修改初始化

#### 原代码
```c
pid_init(&motor_pid, 
         2.5f,    // Kp
         0.1f,    // Ki
         0.05f,   // Kd
         0.001f,  // dt (1ms)
         -100.0f, // output_min
         100.0f); // output_max
```

#### 新代码
```c
#ifdef USE_CMSIS_DSP
// 注意：DSP PID 不需要 dt 参数！
// Ki 和 Kd 已隐含采样时间，需重新调整
pid_dsp_init(&motor_pid, 
             2.5f,    // Kp（不变）
             0.1f,    // Ki（需乘以 dt）
             0.05f,   // Kd（需除以 dt）
             -100.0f, // output_min
             100.0f); // output_max
#else
pid_init(&motor_pid, 2.5f, 0.1f, 0.05f, 0.001f, -100.0f, 100.0f);
#endif
```

**关键转换公式**（如果原实现用了 dt）:
```c
// 假设原 PID 参数：Kp, Ki, Kd, dt
// DSP PID 参数转换：
Kp_dsp = Kp;
Ki_dsp = Ki * dt;  // 积分项隐含采样时间
Kd_dsp = Kd / dt;  // 微分项隐含采样时间
```

### 第 4 步：修改计算调用

#### 原代码
```c
float output = pid_calculate(&motor_pid, target_speed, current_speed);
```

#### 新代码
```c
#ifdef USE_CMSIS_DSP
// DSP PID 需要手动计算误差
float error = target_speed - current_speed;
float output = pid_dsp_calculate(&motor_pid, error);
#else
float output = pid_calculate(&motor_pid, target_speed, current_speed);
#endif
```

### 第 5 步：其他接口迁移

#### 重置 PID
```c
#ifdef USE_CMSIS_DSP
pid_dsp_reset(&motor_pid);
#else
pid_reset(&motor_pid);
#endif
```

#### 更新参数
```c
#ifdef USE_CMSIS_DSP
pid_dsp_update_params(&motor_pid, new_kp, new_ki, new_kd);
#else
pid_update_params(&motor_pid, new_kp, new_ki, new_kd);
#endif
```

---

## 完整迁移示例

### 原代码（module/motion/motor_control.c）
```c
#include "pid/pid.h"

typedef struct {
    pid_controller_t speed_pid;
    float target_speed;
    float current_speed;
} motor_t;

motor_t motors[4];

void motor_init(void) {
    for (int i = 0; i < 4; i++) {
        pid_init(&motors[i].speed_pid, 
                 2.5f, 0.1f, 0.05f,  // Kp, Ki, Kd
                 0.001f,              // dt = 1ms
                 -100.0f, 100.0f);    // 输出限制
    }
}

void motor_control_task(void) {
    for (int i = 0; i < 4; i++) {
        motors[i].current_speed = encoder_get_speed(i);
        
        float pwm = pid_calculate(&motors[i].speed_pid, 
                                   motors[i].target_speed,
                                   motors[i].current_speed);
        
        motor_set_pwm(i, pwm);
    }
}
```

### 迁移后（兼容 DSP 和原实现）
```c
#include "pid/pid.h"

typedef struct {
#ifdef USE_CMSIS_DSP
    pid_controller_dsp_t speed_pid;
#else
    pid_controller_t speed_pid;
#endif
    float target_speed;
    float current_speed;
} motor_t;

motor_t motors[4];

void motor_init(void) {
    for (int i = 0; i < 4; i++) {
#ifdef USE_CMSIS_DSP
        // DSP PID：Ki 和 Kd 需调整
        float kp = 2.5f;
        float ki = 0.1f * 0.001f;  // Ki * dt
        float kd = 0.05f / 0.001f; // Kd / dt
        pid_dsp_init(&motors[i].speed_pid, kp, ki, kd, -100.0f, 100.0f);
#else
        pid_init(&motors[i].speed_pid, 2.5f, 0.1f, 0.05f, 0.001f, -100.0f, 100.0f);
#endif
    }
}

void motor_control_task(void) {
    for (int i = 0; i < 4; i++) {
        motors[i].current_speed = encoder_get_speed(i);
        
#ifdef USE_CMSIS_DSP
        float error = motors[i].target_speed - motors[i].current_speed;
        float pwm = pid_dsp_calculate(&motors[i].speed_pid, error);
#else
        float pwm = pid_calculate(&motors[i].speed_pid, 
                                   motors[i].target_speed,
                                   motors[i].current_speed);
#endif
        
        motor_set_pwm(i, pwm);
    }
}
```

---

## 宏封装（可选，简化代码）

在 `pid/pid.h` 末尾添加（可选）：

```c
#ifdef USE_CMSIS_DSP
// 宏封装：统一接口
#define PID_CONTROLLER_T        pid_controller_dsp_t
#define PID_INIT(p,kp,ki,kd,dt,min,max) \
    pid_dsp_init(p, kp, (ki)*(dt), (kd)/(dt), min, max)
#define PID_CALCULATE(p,sp,fb) \
    pid_dsp_calculate(p, (sp)-(fb))
#define PID_RESET(p)            pid_dsp_reset(p)
#define PID_UPDATE_PARAMS(p,kp,ki,kd) \
    pid_dsp_update_params(p, kp, ki, kd)
#else
#define PID_CONTROLLER_T        pid_controller_t
#define PID_INIT(p,kp,ki,kd,dt,min,max) \
    pid_init(p, kp, ki, kd, dt, min, max)
#define PID_CALCULATE(p,sp,fb)  pid_calculate(p, sp, fb)
#define PID_RESET(p)            pid_reset(p)
#define PID_UPDATE_PARAMS(p,kp,ki,kd) \
    pid_update_params(p, kp, ki, kd)
#endif
```

使用宏后的代码（最简洁）：

```c
PID_CONTROLLER_T motor_pid;

void motor_init(void) {
    PID_INIT(&motor_pid, 2.5f, 0.1f, 0.05f, 0.001f, -100.0f, 100.0f);
}

void motor_task(void) {
    float pwm = PID_CALCULATE(&motor_pid, target, current);
}
```

---

## 注意事项

### 1. 参数转换（重要！）

CMSIS-DSP 的 PID 内部实现为：
```
output = Kp * e[n] + Ki * sum(e) + Kd * (e[n] - e[n-1])
```

而传统离散 PID 为：
```
output = Kp * e[n] + Ki * dt * sum(e) + Kd / dt * (e[n] - e[n-1])
```

因此转换公式：
- `Ki_dsp = Ki_orig * dt`
- `Kd_dsp = Kd_orig / dt`

### 2. 积分限幅

DSP PID 没有独立的积分限幅接口，如需限制，可在调用前后手动处理：

```c
float output = pid_dsp_calculate(&pid, error);

// 手动积分限幅（如果需要）
if (pid.instance.state[2] > integral_max) {
    pid.instance.state[2] = integral_max;
} else if (pid.instance.state[2] < integral_min) {
    pid.instance.state[2] = integral_min;
}
```

但通常输出限幅已足够，无需额外积分限幅。

### 3. 批量 PID（未来优化）

DSP PID 的真正优势在于批量计算（4 电机同时）：

```c
// 未来可扩展为向量化版本
void motor_batch_pid(float errors[4], float outputs[4]) {
    // 使用 DSP 向量指令一次计算 4 组 PID
    // 预计加速比：~2.5-3x
}
```

---

## 调试技巧

### 验证参数转换是否正确

```c
// 测试代码：两种实现输出应一致
pid_controller_t pid_orig;
pid_controller_dsp_t pid_dsp;

float dt = 0.001f;
float kp = 2.5f, ki = 0.1f, kd = 0.05f;

pid_init(&pid_orig, kp, ki, kd, dt, -100, 100);
pid_dsp_init(&pid_dsp, kp, ki*dt, kd/dt, -100, 100);

// 同样的输入，比较输出
float target = 100.0f, feedback = 80.0f;
float out1 = pid_calculate(&pid_orig, target, feedback);
float out2 = pid_dsp_calculate(&pid_dsp, target - feedback);

printf("Orig: %.4f, DSP: %.4f, Diff: %.6f\n", out1, out2, out1 - out2);
// 预期：Diff < 0.001
```

### 性能测试

```c
#include "core_cm7.h"  // DWT 计时

DWT->CYCCNT = 0;
float output = pid_dsp_calculate(&pid, error);
uint32_t cycles = DWT->CYCCNT;

printf("DSP PID cycles: %u\n", cycles);
```

---

## 常见问题

**Q: 为什么 DSP PID 没有 dt 参数？**  
A: CMSIS-DSP 假设 PID 在固定周期调用，Ki/Kd 已包含采样时间。

**Q: 参数转换后控制效果变差了？**  
A: 检查 Ki/Kd 转换公式是否正确，或者调用频率是否改变。

**Q: 可以在运行时切换 DSP/原实现吗？**  
A: 不推荐！结构体大小不同，会导致内存错误。应在编译时选定。

**Q: DSP PID 支持抗积分饱和吗？**  
A: 部分支持，需手动访问 `pid.instance.state[2]`（积分项）并限幅。

---

## 推荐策略

1. **新项目**：直接用 DSP PID（定义 `USE_CMSIS_DSP`）
2. **老项目**：保持原 PID，仅在性能瓶颈时迁移
3. **混合使用**：实时电机 PID 用 DSP，低频位置环用原实现

迁移完成后记得测试闭环响应，确保阶跃响应、超调、稳态误差等指标一致！
