# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

BFCopilot 是仿生蝴蝶扑翼飞控固件，C 语言，**ESP-IDF v6.0.2**，目标 `esp32c6`（见 `sdkconfig.defaults`）。

仓库正在做结构重构：当前只实现了 **BSP + 通用库**，`src/driver`、`src/service`、`src/app` 三层尚未恢复。
根 `CMakeLists.txt` 已把这三层写进 `EXTRA_COMPONENT_DIRS` 并设置 `COMPONENTS app`，所以在 `src/app`
恢复前，根工程无法完成配置和构建。不要为了通过配置而伪造空的 app；验证 BSP 用临时 IDF 工程（见下）。

硬件事实的唯一依据是 `docs/hardware-pinmap.md`（从 EasyEDA 实时原理图「蝴蝶控制器v1」P1 导出，
2026-10-20）。所有文档用中文。

## 版本控制：jj

本仓库用 Jujutsu（`.jj/`，与 `.git/` colocated）。保存、查看、回退一律用 `jj`，
不要用 `git commit` / `git checkout` / `git reset`。

```bash
jj status                  # 工作区状态（改动自动快照，无需 add）
jj diff                    # 当前变更内容
jj log -n 10               # 最近历史
jj commit -m "说明"         # 当前变更定稿，并在其上开一个新的空变更
jj describe -m "说明"       # 只修改当前变更的描述
jj new <rev>               # 在指定提交上开新变更
jj undo                    # 撤销上一次 jj 操作
```

提交信息用中文，说明做了什么、为什么。`jj status` 显示没有改动即已保存。

## 构建与烧录

本机 IDF：`C:\Users\yuang\Projects\Espidf\v6.0.2\esp-idf`（`.vscode/settings.json` 已指向它）。

PowerShell（推荐）：

```powershell
& C:\Users\yuang\Projects\Espidf\v6.0.2\esp-idf\export.ps1
idf.py build
idf.py -p COM4 flash monitor
idf.py menuconfig
```

git-bash 下 `idf.py` 会因 `MSYSTEM` 环境变量跳过 `main()`，必须先 `unset MSYSTEM`。
旧版 `scripts/idf.sh` / `scripts/idf.cmd` 已经删除（`scripts/` 目前为空），不要再引用。

**只验证 BSP 时**（根工程还缺 `src/app`）：

1. 新建一个临时 ESP-IDF 工程；
2. `EXTRA_COMPONENT_DIRS` 指向本仓库的 `src/lib` 和 `src/bsp`；
3. `sdkconfig.defaults` 写入 `CONFIG_IDF_TARGET="esp32c6"`；
4. `main` 里调用各 `bsp_*_init()` / `bsp_*_read()` 并链接 `REQUIRES bsp lib`。

这样能在不动 `src/app` 的情况下编译、链接全部 BSP。`build/` 是旧配置产物（已 gitignore），
clangd 通过 `.vscode/settings.json` 使用其中的 `compile_commands.json`。

也可以使用 `.devcontainer/`（基于 `espressif/idf` 镜像）。

## 主机工具

PATH 上的 `python` 是 Windows Store 桩，统一用 `uv`：

```bash
uv run --project tools python tools/bf_ground.py --list
uv run --project tools python tools/bf_station.py
```

`tools/bf_station.py` 是 Tkinter 台架 UI，`tools/bf_ground.py` 是串口地面站；
两者面向旧的 console/协议固件，那部分在当前重构中尚未恢复。

## 架构与组件

### `src/lib/` —— 纯逻辑库（不依赖 ESP-IDF 外设）

- `tools/`：`exit_code_t` 返回码、`u8/u16/u32/f32`、`error_code_name()`；`vec3f` 向量运算。
- `buffer/`：`fifo_buffer`，可选互斥回调。`FIFO_BUFFER_USING_MUTEX` 会改变结构体布局，
  全工程该宏必须一致。
- `filter/`、`pid/`：从另一工程迁入，仍引用 `module/sys/sys_log.h`、`zf_common_headfile.h`，
  这些头在本仓库不存在；**目前不在 `src/lib/CMakeLists.txt` 的编译列表里**。接入前先决定日志接口。

### `src/bsp/` —— BSP + 设备驱动（用户明确要求合成一层）

| 模块 | 职责 | 关键点 |
|---|---|---|
| `adc/` | 电池电压、舵机电流 | ADC1 oneshot + 逐通道曲线校准；只返回 raw / mV，分压比和采样电阻不在 BSP 里换算 |
| `baro/` | SPA06-003 气压计 | I2C 100 kHz；0x76/0x77 双地址探测并用 ID（0x0D=0x11）校验；`spa06_math.c` 是纯计算补偿 |
| `buzze/` | 蜂鸣器 | LEDC 10 bit，初始化后 0 占空比静音 |
| `imu/` | LSM6DS3TR-C | `vendor/` 是 ST 官方 PID 2.2.3，**禁止修改**；SPI 用 `command_bits=8` 让 CS 覆盖地址+数据；固定 ±2 g / ±250 dps |
| `servo/` | 双路舵机 | MCPWM 50 Hz、1 µs 分辨率；上电输出强制低、舵机电源默认关断 |

### 规划中的层

`src/driver`、`src/service`、`src/app` 已出现在根 CMake 里但目录还不存在。
恢复时保持依赖方向 **app → service → driver → bsp/lib**，不要把业务逻辑塞进 BSP。

## 代码约定

- 风格沿用 `CNC_Power_Supply`：`exit_code_t` 返回码、`u8/u16/u32/f32`、中文注释解释“为什么”。
- **不要新建 `xxx_config.h`**：引脚和参数宏直接放进模块的 `bsp_xxx.h`（用户明确要求）。
- 每个顶层目录是一个 ESP-IDF 组件，用 `idf_component_register` 注册；include 写成
  `"bsp_adc.h"` 或 `"lib/tools/common_def.h"`。
- 公共头不要暴露驱动类型（例如 `I2C_NUM_0` 只放在 `.c` 里），避免使用方被迫依赖 ESP-IDF 头。

## 硬件与安全约束

- 引脚唯一来源：`docs/hardware-pinmap.md`。改引脚先改文档，再改代码。
- 舵机电源开关存在：`GPIO2(SERVO_EN)` → Q3 → Q2 → `VCC_Servo`。
  `bsp_servo_init()` 第一件事就是把它拉低。
- 停止 PWM 不等于断电；要断电调用 `bsp_servo_power_enable(false)`，它会同时把两路输出压回低电平。
- 未测量/未标定的量（分压比、采样电阻值、舵机端点、电池阈值）保持参数化，不得写死或当作飞行限制。
- 气压计 SDO 悬空，I2C 地址不固定，必须运行时探测 0x76/0x77。
- `src/bsp/imu/vendor/` 下的 ST 官方代码不要改；升级时整目录替换，并更新
  `src/bsp/imu/README.md` 里的版本与提交记录。

## 测试

当前没有主机测试或 Python 测试目录（旧 `test/` 已随重构删除）。BSP 的编译验证用上面的
临时工程；`spa06_math`、`vec_math`、`fifo_buffer` 这类纯计算/纯逻辑代码适合后续补主机测试。
