# 硬件基线与标定状态

> 文档状态：阶段 0 进行中。**所有标注“未实测”的项都必须在带翼通电前闭合。**
>
> 本文件记录当前实物的连线、电源与测量结果。没有实测记录的数值一律保持“未测量”，
> 不得作为飞行限制或过流/欠压阈值使用。

## 1. 资料与可信度

资料根目录：`C:\Users\yuang\Projects\Hardware\BufferFly\_handoff`（尚未在本仓库内
逐项核对）。

| 来源 | 用途 | 限制 |
|---|---|---|
| `netlist/bom.csv` | 器件清单 | 不含完整连接 |
| `netlist/netlist.txt`、`netlist.json` | 候选 GPIO 映射 | 与旧资料有时间版本差异 |
| `hardware-review.md` | 历史审查与上电清单 | 是审查结论，非当前板整改证明 |
| `README.md` | 历史交接背景 | 自带旧快照问题 |
| `datasheets/` | 寄存器/电气范围 | 舵机手册未提供 |

## 2. 实物版本

| 项目 | 值 | 状态 |
|---|---|---|
| 板名 | butterfly-current | — |
| 硬件版本号 | 未记录 | **待补充（拍照/丝印）** |
| 原理图/网表版本 | 蝴蝶控制器v1 / P1 | 2026-10-13 经 easyeda-agent 在线读取器件引脚与连接图，见 §9 |
| 固件基线 | ESP-IDF v6.0.2，ESP32-C6 | 已确认 |

## 3. 引脚（已在固件中集中定义，见 `src/bsp/board/include/board_pins.h`）

| 功能 | GPIO | 实测确认 |
|---|---:|---|
| 左舵机 PWM | 18 | 原理图：U1 焊盘24 → PWM_L → H1 第3脚；波形未实测 |
| 右舵机 PWM | 19 | 原理图：U1 焊盘25 → PWM_R → H2 第3脚；波形未实测 |
| IMU SCK/MOSI/MISO/CS | 7/15/14/6 | 在线原理图已核对 LSM6DS3TR-C；实物未测；通信失败见 §8 |
| 气压计 SCL/SDA | 4/5 | 在线原理图已核对 SPA06-003；用户确认 CSB/SDO 悬空；采样未测；通信失败见 §8 |
| 电池 ADC | 0 | 未实测分压比 |
| 电流 ADC | 1 | 未实测分流/增益 |
| 蜂鸣器 | 3 | 未实测 |
| WS2812 | 23 | 未实测 |
| USB DM/DP | 12/13 | 未确认模块 USB 能力 |

## 4. 阶段 0 待闭合清单

- [ ] 用万用表确认 U1 焊盘24/25（GPIO18/19）到 H1/H2 第3脚，H1/H2 第1脚为 GND、第2脚为 VCC_Servo；再测 PWM 波形。
- [ ] 确认 2S 电池直接接入路径与满电实测电压；记录电池型号、容量、截止电压。
- [ ] 实测电池 ADC 分压比（写入 `sensors.vbat_div_num/den`），确认满电读数不越 ADC 量程。
- [ ] 实测分流电阻与 INA199 增益、零电流输出（写入 `sensors.iservo_shunt_mohm` /
      `sensors.ina199_gain` / `sensors.iservo_zero_mv`）。
      **注意**：BOM 中 R11 写作 10 mΩ 而 MPN 含 `R030`，历史审查按 30 mΩ 分析；
      不得直接采用任一值。
- [ ] 记录舵机空载/带载电流边界；堵转测试仅在有电流限制、明确时长、可人工断电时执行。
- [ ] 确认舵机允许电压不低于实际 `VCC_Servo`（用户确认 8.4 V，仍需手册/铭牌补证）。

## 5. 阻断项（Blocker）

以下情况出现时**停止电池动力测试**，仅继续断电验证与纯软件工作，禁止以“会用软件
保护”为由继续上电：

- 实物 ADC 满电过压或读数不可区分；
- 电源不稳定或载流能力不足；
- 舵机电源/信号连接未确认。

## 6. 安全规则

- 首次测试拆下连杆或使用限位工装，**禁止带翅膀通电**。
- 上电永远锁定；未标定前禁止带翼解锁（固件已强制）。
- 当前板**没有舵机电源 MOS 控制**：停止 PWM 不等于断电。状态上报 `NO_POWER_CUTOFF`。

## 7. 修订记录

| 日期 | 变更 | 操作者 |
|---|---|---|
| 2026-10-08 | 建档，全部测量项标为未实测 | — |
| 2026-10-13 | 记录 U3/U5 首次通信失败现象与待测项（见下） | — |
| 2026-10-13 | 在线核对全板软件相关引脚，区分模块焊盘与 GPIO，修复持续强制低电平的释放方式 | Codex |

## 8. 传感器通信失败记录（2026-10-13）

两片传感器均无应答，且在手写驱动与 ST 官方驱动下现象一致，已排除驱动实现问题。

| 观测 | 值 | 判读 |
|---|---|---|
| IMU SPI 事务状态 | `transfer=ESP_OK` | 主机侧时序正常；SPI 无应答检测，不能证明从设备响应 |
| IMU `WHO_AM_I` | `0x00`（连读三次） | 芯片未应答 |
| IMU MISO(GPIO14) 空闲电平 | `0` | 该线未被有效驱动；无上拉时高阻悬空也可能读到 0，单独不足以定性 |
| I²C SCL/SDA 空闲电平 | `1` / `1` | 上拉与走线健康 |
| I²C 0x77 / 0x76 | 均 `ESP_ERR_NOT_FOUND` | 非 SDO 悬空导致的地址选择问题 |
| I²C 其他非保留地址 | 0 个应答 | 总线上无任何从设备 |

主导假设：U3 与 U5 **共用供电未到位**（VDD/VDDIO 无 3.3 V），一次解释两条独立总线同时失效；
其次为两片各自焊接/连线问题（SDO 短地、虚焊）。

**待实测（尚未进行）**：

- [ ] U3（LSM6DS3TR-C）VDD、VDDIO 对地电压；U5（SPA06-003）VDD、VDDIO 对地电压。
- [ ] U3/U5 的地与 ESP32 是否共地。
- [ ] GPIO6(CS)/7(SCK)/15(MOSI) 在身份读取期间是否有 1 MHz 时钟、CS 拉低、`0x8F` 读地址（示波器/逻辑分析仪）。
- [ ] GPIO14(MISO) 到 U3 SDO 的连通性，以及是否短路到地。
- [ ] U5 CSB/SDO 实际电平与到芯片引脚的连通性（此前记为"悬空"，需实测确认）。

在以上测量闭合前，`IMU_LOST` / `BARO_LOST` 与 `FAULT_LATCHED` 是固件的预期安全行为，不是缺陷。

## 9. 在线原理图回读与 PWM 排查（2026-10-13）

来源为 EasyEDA Pro 4.1.71 的「蝴蝶控制器v1」，工程 UUID
`4d262c13a2b54dc1a8ee88867de57bd2`，P1 UUID `3677e7b5e464dcf3`。
通过 `easyeda sch list --include-pins --include-wires` 与 `sch connectivity` 回读，
确认返回目标页身份及可用网表；原始快照保存在 `.cache/hardware-schematic.json` 和
`.cache/hardware-connectivity.json`。CLI/daemon 为 1.11.0、选用连接器为 1.9.1，
版本存在诊断差异；本轮仅使用已有读取接口，未修改原理图或 PCB。

U1 型号 ESP32-C6-MINI-1U-H4；**符号的 pinNumber 是模块焊盘号，pinName 的 IOxx 才是固件 GPIO**。

| 功能 | U1 焊盘 | GPIO / 引脚名 | 网络及外围端点 |
|---|---:|---|---|
| 左 PWM | 24 | GPIO18 / IO18 | PWM_L → H1.3 |
| 右 PWM | 25 | GPIO19 / IO19 | PWM_R → H2.3 |
| IMU CS | 15 | GPIO6 / IO6 | IMU_CS → U3.12 |
| IMU SCK | 16 | GPIO7 / IO7 | IMU_SCK → U3.13 |
| IMU MISO | 19 | GPIO14 / IO14 | IMU_MISO → U3.1 |
| IMU MOSI | 20 | GPIO15 / IO15 | IMU_MOSI → U3.14 |
| 气压计 SCL | 9 | GPIO4 / IO4 | I2C_SDL（原图命名）→ U5.4 |
| 气压计 SDA | 10 | GPIO5 / IO5 | I2C_SDA → U5.3 |
| 电池 ADC | 12 | GPIO0 / IO0 | ADC_Battery |
| 电流 ADC | 13 | GPIO1 / IO1 | ADC_Current |
| 辅助 ADC（当前未驱动） | 5 | GPIO2 / IO2 | ADC_Aux |
| 蜂鸣器控制 | 6 | GPIO3 / IO3 | BUZZER，经外围驱动 |
| 左 WS2812 链 | 29 | GPIO23 / IO23 | LED_L → LED1.4(DI)，LED1.1(DO) → LED_L1 |
| 右 LED 网络（当前未驱动） | 28 | GPIO22 / IO22 | LED_R |
| USB D− / D+ | 17 / 18 | GPIO12 / GPIO13 | USB_DN / USB_DP |
| ELRS 接口（当前未驱动） | 26 / 27 | GPIO20 / GPIO21 | ELRS_TX / ELRS_RX |

H1/H2 均为第1脚 GND、第2脚 VCC_Servo、第3脚 PWM。U3 的 VDD/VDDIO 为
第8/5脚、U5 为第8/6脚，网表均接 3V3；U5 的 CSB(2)/SDO(5) 明确标 NC。
R11 仍有 Value=10mΩ 与型号 MFJ06HR030FT 的矛盾，不能据原理图填入测量参数。

无波形的软件原因：`drive_off()` 使用 MCPWM 的持续强制低电平（`hold_on=true`），
旧 `drive_on()` 用 `hold_on=false` 解除的是非持续模式。ESP-IDF 6.0.2 源码中两者
控制不同强制动作，因此旧代码即使返回 ESP_OK，也保持持续低电平。
现改为 `mcpwm_generator_set_force_level(gen, -1, true)` 解除同一持续模式。

刷入修复后的台架固件，执行 `hw pulse both 1550`，示波器探头接 H1/H2 第3脚，
参考地接第1脚；预期约 333 Hz、周期 3003 µs、高电平 1550 µs。
`hw pulse off` 后应持续低电平，再次发送应恢复。
软件回归测试不代表实物波形已测得；若 U1 有波形而 H1/H2 无波形，继续断电查两端连通性。

