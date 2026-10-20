# 硬件引脚与连接（蝴蝶控制器 v1）

本文档是固件 `board_pins.h` 的唯一依据：任何引脚、网络或电源拓扑的改动必须先改这里，再改代码。

- 依据：EasyEDA Pro 4.1.71，工程「蝴蝶控制器v1」，原理图页 `P1`（document UUID `3677e7b5e464dcf3`）
- 采样方式：easyeda-agent 连接器 `sch list --include-pins` / `sch connectivity`，2026-10-20
- 性质：**原理图事实**，不是台架实测。凡标注「待确认」的条目，在实测前不得当作飞行限制或标定值。

## 1. 电源拓扑

```
USB-C VBUS ──D1(SS34)──┬────────────────────────────── +5V ──┬── LDO1 ME6211C33 ── 3V3
                        │                                     ├── LED1 VDD (WS2812)
                        │                                     ├── LED2 (5V 指示)
                        │                                     ├── R2 (U2 FB 上臂)
                        │                                     └── U6-1 (外部灯带供电)
                        │
VIN (电池，见 §7.1) ────┴── U2 ME3116AM6G ──L2 2.2µH── +5V
                     │        EN 经 R1 上拉到 VIN，FB = R2 51K / R3 10K
                     ├── R13 20K ── $1N137 ── R14 10K ── GND
                     │                └── R15 10K ── ADC_Battery（C15 100nF 到地）
                     ├── R11 采样电阻 ── $1N185
                     │        ├── U4 INA199A1: IN+ = VIN, IN- = $1N185, OUT ─ R12 10K ─ ADC_Current（C16 100nF）
                     │        └── Q2 AO3401A(P): S = $1N185, D = VCC_Servo ── H1-2 / H2-2
                     │                栅极 $1N177 经 R18 10K 上拉到 VIN，由 Q3 AO3400A(N) 拉低
                     │                Q3: G = SERVO_EN, S = GND, D = $1N177
                     └── R18 / U2.IN / U4.IN+ / C3 10µF
```

要点：

- **舵机电流单独过采样电阻**。`U2`（降压）、`R13`（电池分压）和 `R18`（栅极上拉）都接在采样电阻的 VIN 侧，
  因此 `ADC_Current` 只反映 `VCC_Servo` 的电流，不含 MCU 自身功耗。
- **舵机电源 = 电池电压**，不是 5V。`VCC_Servo` 取自 `$1N185`（VIN 经采样电阻），经 Q2 高边开关送到 H1/H2 的 2 脚。
- **舵机电源有使能开关**：`SERVO_EN`(GPIO2) 为高 → Q3 导通 → Q2 栅极被拉低 → Q2 导通 → `VCC_Servo` 上电。
  这与旧版 `board_pins.h`/旧硬件文档中「本板无舵机电源开关」的结论相反，旧结论作废。
- 5V 由 `ME3116AM6G` 从 VIN 降压得到，使能脚经 R1 常上拉，**上电即工作**。
- 3V3 由 `ME6211C33M5G-N` 从 5V 降压得到，CE 直接接 5V。

## 2. MCU：U1 ESP32-C6-MINI-1U-H4

模块焊盘号与 GPIO 编号不是一回事，固件里只能用 GPIO 编号。

| 焊盘 | 引脚名 | GPIO | 网络 | 方向 | 用途 |
|---:|---|---:|---|---|---|
| 1, 2, 11, 14, 36–53 | GND | — | GND | 电源 | 地 |
| 3 | 3V3 | — | 3V3 | 电源 | 3.3V 供电 |
| 4, 7, 21, 32–35 | NC | — | — | — | 模块未引出 |
| 5 | IO2 | 2 | SERVO_EN | 输出 | 舵机电源使能，高有效 |
| 6 | IO3 | 3 | BUZZER | 输出 | 蜂鸣器驱动（经 Q1 低边） |
| 8 | EN | — | MCU_EN | 输入 | 芯片复位，R4 10K 上拉 + C7 1µF + C9 100nF + SW4 到地 |
| 9 | IO4 | 4 | I2C_SDL | 双向 | 气压计 I2C SCL（R8 10K 上拉） |
| 10 | IO5 | 5 | I2C_SDA | 双向 | 气压计 I2C SDA（R9 10K 上拉） |
| 12 | IO0 | 0 | ADC_Battery | 模拟输入 | 电池电压分压，ADC1_CH0 |
| 13 | IO1 | 1 | ADC_Current | 模拟输入 | INA199 输出，ADC1_CH1 |
| 15 | IO6 | 6 | IMU_CS | 输出 | LSM6DS3TR-C 片选 |
| 16 | IO7 | 7 | IMU_SCK | 输出 | IMU SPI 时钟 |
| 17 | IO12 | 12 | USB_DN | 双向 | 原生 USB D− |
| 18 | IO13 | 13 | USB_DP | 双向 | 原生 USB D+ |
| 19 | IO14 | 14 | IMU_MISO | 输入 | IMU SPI MISO（SDO/SA0） |
| 20 | IO15 | 15 | IMU_MOSI | 输出 | IMU SPI MOSI（SDA） |
| 22 | IO8 | 8 | $1N65 | — | 仅 R5 10K 上拉到 3V3，原理图未赋予功能 |
| 23 | IO9 | 9 | BOOT | 输入 | BOOT 按键 SW3 到地，C8 100nF；无外部上拉 |
| 24 | IO18 | 18 | PWM_L | 输出 | 左舵机 PWM → H1-3 |
| 25 | IO19 | 19 | PWM_R | 输出 | 右舵机 PWM → H2-3 |
| 26 | IO20 | 20 | ELRS_TX | 输出 | 外接接收机 UART TX |
| 27 | IO21 | 21 | ELRS_RX | 输入 | 外接接收机 UART RX |
| 28 | IO22 | 22 | LED_R | 输出 | 外部灯带数据 R → U6-2 |
| 29 | IO23 | 23 | LED_L | 输出 | 板载 WS2812 DI，DO 转发到 U6-3 |
| 30 | RXD0 | — | RX | 输入 | UART0 接收 → U7-2 |
| 31 | TXD0 | — | TX | 输出 | UART0 发送 → U7-1 |

## 3. 外设连接

### 3.1 IMU：U3 LSM6DS3TR-C（SPI，4 线）

| 引脚 | 名称 | 网络 | 说明 |
|---:|---|---|---|
| 1 | SDO/SA0 | IMU_MISO | SPI MISO |
| 2 | SDx | GND | 接地下拉，SPI 4 线模式 |
| 3 | SCx | GND | 接地下拉，SPI 4 线模式 |
| 4 | INT1 | NC | 未使用 |
| 5 | VDDIO | 3V3 | IO 电源 |
| 6, 7 | GND | GND | — |
| 8 | VDD | 3V3 | 主电源 |
| 9 | INT2 | NC | 未使用 |
| 10, 11 | NC | — | — |
| 12 | CS | IMU_CS | 片选低有效 |
| 13 | SCL | IMU_SCK | SPI 时钟 |
| 14 | SDA | IMU_MOSI | SPI MOSI |

### 3.2 气压计：U5 SPA06-003（I2C）

| 引脚 | 名称 | 网络 | 说明 |
|---:|---|---|---|
| 1, 7 | GND | GND | — |
| 2 | CSB | NC | 悬空 |
| 3 | SDA | I2C_SDA | — |
| 4 | SCK | I2C_SDL | — |
| 5 | SDO | NC | **悬空**，地址选择未固定，见 §7.2 |
| 6 | VDDIO | 3V3 | — |
| 8 | VDD | 3V3 | — |

### 3.3 电流检测：U4 INA199A1DCKR + R11

| 引脚 | 名称 | 网络 |
|---:|---|---|
| 1 | REF | GND（单向测量，0V 基准） |
| 2 | GND | GND |
| 3 | V+ | 3V3 |
| 4 | IN+ | VIN |
| 5 | IN− | $1N185（采样电阻负载侧） |
| 6 | OUT | $1N172 ─ R12 10K ─ ADC_Current |

`INA199A1` 增益 50 V/V。R11 为 1206 采样电阻 `MFJ06HR030FT`。换算见 §7.3。

### 3.4 舵机输出

| 接口 | 1 | 2 | 3 |
|---|---|---|---|
| H1（左） | GND | VCC_Servo | PWM_L（GPIO18） |
| H2（右） | GND | VCC_Servo | PWM_R（GPIO19） |

`VCC_Servo` 来自 Q2 高边开关（见 §1），由 `SERVO_EN`(GPIO2) 控制。舵机地回到板 GND。

### 3.5 蜂鸣器

```
3V3 ── BUZZER1(+)      BUZZER1(-) ── $1N123 ── Q1(AO3400A_N).D
                                              Q1.S ── GND
GPIO3(BUZZER) ── R10 1K ── Q1.G($1N174)
3V3 ── D3(1N4148W).K      D3.A ── $1N123（续流）
```

- 有源/无源类型未确认（BOM `Value` 字段写 4kHz），见 §7.5。
- Q1 栅极只有 1K 串联电阻，**没有下拉**，见 §7.4。
- BUZZER1 的 3 脚标称 NC，实际接 GND（机械脚）。

### 3.6 LED

| 器件 | 连接 | 说明 |
|---|---|---|
| LED1 XL-1615RGBC-2812B | VDD=+5V, GND=GND, DI=LED_L(GPIO23), DO=LED_L1 | 板载 WS2812，DO 串到 U6-3 |
| U6-2 | LED_R(GPIO22) | 外部灯带头，独立数据脚 |
| LED2 LED_0805-G | A=+5V, C ─ R16 1K ─ GND | 5V 电源指示 |
| LED3 LED_0805-G | A=3V3, C ─ R17 1K ─ GND | 3V3 电源指示 |

### 3.7 USB：USB1 TYPE-C 16PIN

| 引脚 | 网络 | 说明 |
|---|---|---|
| A4B9 / B4A9 (VBUS) | $1N152 ─ D1(SS34) ─ +5V | VBUS 经肖特基给 5V 供电，防反灌 |
| A1B12 / B1A12 / 13 / 14 | GND | — |
| A6 / B6 (Dp) | USB_DP | GPIO13 |
| A7 / B7 (Dn) | USB_DN | GPIO12 |
| A5 (CC1) ─ R7 5.1K ─ GND | — | UFP 下拉 |
| B5 (CC2) ─ R6 5.1K ─ GND | — | UFP 下拉 |
| A8 / B8 (SBU) | NC | — |

D+/D− 上没有 ESD 器件，见 §7.6。

### 3.8 连接器

| 器件 | 封装 | 1 | 2 | 3 | 4 | 5, 6 |
|---|---|---|---|---|---|---|
| U6 外部灯带 | GH1.25-4P | +5V | LED_R | LED_L1 | GND | GND（固定） |
| U7 调试串口 | GH1.25-4P | TX | RX | 3V3 | GND | GND（固定） |

### 3.9 按键

| 器件 | 网络 | 说明 |
|---|---|---|
| SW3 | BOOT ↔ GND | 按住上电进下载模式；靠芯片内部上拉 |
| SW4 | MCU_EN ↔ GND | 复位 |

## 4. 网络清单

| 网络 | 连接 |
|---|---|
| 3V3 | LDO1.VOUT、U1、U3、U5、U4.V+、BUZZER1(+)、U7-3、R4/R5/R8/R9 上拉、LED3、D3 阴极 |
| +5V | LDO1.VIN/CE、L2、D1 阴极、LED1/LED2、R2、U6-1 |
| VIN | U2.IN、R1、R11、R13、R18、U4.IN+、C3（**无外部接入点**，见 §7.1） |
| VCC_Servo | Q2.D、H1-2、H2-2 |
| GND | 全局 |
| SERVO_EN | U1.IO2、Q3.G |
| BUZZER | U1.IO3、R10 |
| I2C_SDL / I2C_SDA | U1.IO4/IO5、U5.SCK/SDA、R8/R9 |
| IMU_CS / IMU_SCK / IMU_MISO / IMU_MOSI | U1 ↔ U3 |
| ADC_Battery / ADC_Current | U1.IO0/IO1、C15/C16 |
| PWM_L / PWM_R | U1.IO18/IO19、H1-3/H2-3 |
| LED_L / LED_L1 / LED_R | U1.IO23 → LED1.DI；LED1.DO → U6-3；U1.IO22 → U6-2 |
| USB_DN / USB_DP | USB1 ↔ U1.IO12/IO13 |
| TX / RX | U1.TXD0/RXD0 ↔ U7 |
| MCU_EN | U1.EN、R4、C7、C9、SW4 |
| BOOT | U1.IO9、C8、SW3 |

## 5. 固件引脚宏（已实现）

引脚宏分散在各 BSP 模块头里，与模块一一对应：

```c
/* src/bsp/servo/bsp_servo.h */
#define BSP_SERVO_LEFT_GPIO         18
#define BSP_SERVO_RIGHT_GPIO        19
#define BSP_SERVO_POWER_GPIO        2

/* src/bsp/imu/bsp_imu.h */
#define BSP_IMU_PIN_SCK             7
#define BSP_IMU_PIN_MOSI            15
#define BSP_IMU_PIN_MISO            14
#define BSP_IMU_PIN_CS              6

/* src/bsp/adc/bsp_adc.h */
#define BSP_ADC_VBAT_GPIO           0
#define BSP_ADC_ISERVO_GPIO         1

/* src/bsp/buzze/bsp_buzzer.h */
#define BSP_BUZZER_GPIO             3
```

UART0 使用模块的 `TXD0/RXD0`（焊盘 31/30），不要用 GPIO 号硬编码。
LED_R(22)、LED_L(23)、ELRS(20/21)、气压计 I2C(4/5) 尚未有 BSP 模块，
等 `com` 或对应外设模块落地时再加。

## 6. 与旧版 `board_pins.h` 的差异

| 项 | 旧值 | 现状 |
|---|---|---|
| 舵机电源开关 | 不存在，`has_servo_power_switch = false` | **存在**：GPIO2 → Q3 → Q2 → VCC_Servo |
| 辅助 LED 数据 | 无 | GPIO22 → U6-2 |
| 其余 GPIO | 18/19、6/7/14/15、4/5、0/1、3、23、12/13 | 与原理图一致 |

旧硬件文档中「无舵机电源开关」的结论必须同步修正；`board_caps()` 的
`has_servo_power_switch` 应改为 true，`board_servo_power_*()` 不再返回
`ESP_ERR_NOT_SUPPORTED`。

## 7. 待确认 / 风险项

### 7.1 VIN 没有外部接入点

原理图连通性中 `VIN` 只连接 C3、R1、R11、R13、R18、U2.IN、U4.IN+，**没有任何接插件或焊盘**。
USB 只给 `+5V`，不经过 VIN。这意味着按当前原理图，电池和舵机电源没有物理入口。
需要确认是漏画了电池座/焊盘，还是打算通过 PCB 焊盘直接接入。在此之前，`SERVO_EN`
打开也没有电源可送。

### 7.2 气压计地址未固定

SPA06-003 的 SDO 引脚悬空，I2C 从地址在 0x76/0x77 之间不确定。固件必须在初始化时
探测两个地址，不能写死 `0x77`。

### 7.3 采样电阻值存在 BOM 冲突

- 器件型号 `MFJ06HR030FT` 与描述为 **30 mΩ**（1 W，±1%）
- 该实例的属性 `Value` 写成 **10 mΩ**

两者相差 3 倍，直接影响电流换算：

| R11 | 灵敏度 | 3.3V 满量程 |
|---|---|---|
| 30 mΩ | 1.5 V/A | ≈ 2.2 A |
| 10 mΩ | 0.5 V/A | ≈ 6.6 A |

实测前，`ADC_Current` 的换算系数只能作为未标定参数（`calibrated = 0`）。

### 7.4 电源开关与蜂鸣器的栅极没有下拉

- `SERVO_EN` 直接接 Q3 栅极，无下拉。复位/高阻期间 Q3 状态不确定，可能自行打开舵机电源。
- Q1（蜂鸣器）栅极只有 R10 串联，无下拉，栅极电荷会让蜂鸣器在复位期间保持导通。

建议硬件加下拉；固件侧必须在初始化时立即把 GPIO2/GPIO3 驱动为低，并在进入
`board_init()` 之前不依赖外部状态。

### 7.5 蜂鸣器类型

BOM `Value` 为 4kHz，但未区分有源/无源。若为无源，需要 PWM 输出方波；若为有源，
普通 GPIO 电平即可。供电为 3V3，经 Q1 低边驱动。

### 7.6 USB 无 ESD 保护

D+/D− 从 Type-C 直接进模块，没有 TVS。台架使用可以接受，量产前应评估。

### 7.7 WS2812 电平

LED1 供电 5V，数据来自 3.3V GPIO。WS2812 的 VIH 通常为 0.7×VDD = 3.5V，处于临界
状态，长线或低温下可能不稳定。若点灯异常，优先怀疑此处。

### 7.8 电池分压与量程

R13 20K / R14 10K，分压比 3.0（R15 10K 串入 ADC，忽略 ADC 漏电流），3.3V 满量程对应
约 9.9V。电池节数、满电电压与分压电阻精度均未实测，电池电压阈值只能作为未标定参数。

### 7.9 strapping 引脚

GPIO8（R5 上拉，功能未分配）和 GPIO9（BOOT）是 ESP32-C6 的启动 strapping 引脚；
GPIO4/GPIO5/GPIO15 也属于 strapping 引脚，分别接了 I2C 上拉和 IMU MOSI。
上电/复位瞬间的电平需按 C6 数据手册逐项复核。
