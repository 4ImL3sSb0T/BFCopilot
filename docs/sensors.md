# IMU 与气压计

## 硬件依据

2026-10-11 通过 easyeda-agent 1.9.0 对 Web EasyEDA Pro 4.1.71 的实时原理图只读回查：
工程 `蝴蝶控制器v1`（`4d262c13a2b54dc1a8ee88867de57bd2`），P1（`3677e7b5e464dcf3`）。

| 器件 | 接口 | GPIO / 网络 |
|---|---|---|
| U3 LSM6DS3TR-C，C967633 | SPI2，mode 3，1 MHz | SCK=7，MOSI=15，MISO=14，CS=6 |
| U5 SPA06-003，C30589048 | I²C0，100 kHz | SCL=4（网名 `I2C_SDL`），SDA=5 |

用户确认 U5 CSB、SDO 实物悬空，IMU 先输出芯片 XYZ 坐标。SPA06-003 手册允许 CSB
悬空（内置上拉），默认 I²C 地址为 0x77。驱动优先探测 0x77，再检查 0x76，必须读到
ID=0x11 才配置。原理图 R8/R9 分别为 SCL/SDA 提供 10 kΩ 上拉至 3V3。
IMU 的 WHO_AM_I=0x6A；这不是其 I²C 地址。

## 采样与单位

- IMU：104 Hz，±2 g、±250 °/s；软件复位、带超时等待、BDU、地址递增、配置回读。
  读取 data-ready 后突发读取六轴，输出加速度 mg、角速度 mdps（毫度/秒）。
- 气压计：压力和温度均 25 Hz；压力 8 倍、温度 4 倍过采样，内部温度源。
  两种转换合计约 23.2 ms，低于每帧 40 ms；不需要结果右移。复位后有界等待
  SENSOR_RDY/COEF_RDY，一次读取全部 21 字节工厂系数，包括 c31、c40。
  分别检查温度/压力 ready，保留最近温度参与四阶压力补偿。输出 Pa、毫摄氏度。
- 监控任务每 10 ms 调用 `sensors_sample_motion()`，总线访问在 `bf_runtime_lock()`
  之外，避免 I²C 超时持有舵机控制锁。数据用短临界区发布。
- I/O 失败立即将相应读数标为无效；IMU 超过 100 ms、气压计超过 250 ms 无新样本也无效。
  保留最后数值及时间戳供诊断，只有 `*_valid` 为真才可使用。
- 初始化失败只禁用对应传感器并记录原因，ADC 和安全锁定逻辑继续工作。修复启动连接问题后重启。

当前没有机体坐标变换、用户零偏标定、姿态融合或高度控制。
启动自检最多等待 400 ms 获取新鲜传感器数据；身份/配置/采样任一失败会报告
`IMU_LOST` / `BARO_LOST` 并进入 `FAULT_LATCHED`，阻止解锁。运行时也持续检测过期或通信失败。
`sensors.calibrated` 仍只表示电池/电流测量参数已标定，不能当作 IMU 已标定。
无线 STATUS 协议保持既有格式，新增数据可从串口 `status` 和每秒日志查看。

## 验证

纯 C 主机测试覆盖有符号位域、IMU 字节顺序/量纲/角速度范围、完整系数解包、
正负原始值及 c31/c40 补偿、无效系数和超范围结果。
驱动模拟测试覆盖实际驱动代码的芯片 ID 拒绝、复位/系数等待超时、0x76 地址回退、
配置回读、温压异步就绪、样本过期、I/O 失败与下一次正常采样恢复。

```powershell
cmake -S test/host -B test/host/build
cmake --build test/host/build
ctest --test-dir test/host/build --output-on-failure
scripts\idf.cmd build
```

本次验证状态为 `offline-verified`：四组 CTest 全部通过，ESP-IDF v6.0.2 的 ESP32-C6
完整构建通过。因原 `build` 目录 Ninja 记录的权限问题，本次固件在独立目录
`build-sensors` 构建，生成 `build-sensors/BFCopilot.bin`（Flash 配置仍为原来的 2 MB）。
尚未烧录或验证实物传感器数据。烧录本次固件并进入串口监视：

```powershell
scripts\idf.cmd -B build-sensors -p COM4 flash monitor
```

烧录后检查两个初始化成功日志，在 `bf>` 输入 `status`：静止加速度向量长度应约 1000 mg，
角速度接近零；气压应在 30000–110000 Pa 范围，温度应合理。翻转板子检查各轴符号，
断开传感器检查有效标志失效。实物采样、安装方向、噪声与零偏仍需台架验证。

## 寄存器参考

- [ST LSM6DS3TR-C 数据手册](https://www.st.com/resource/en/datasheet/lsm6ds3tr-c.pdf)
- [ST AN5130](https://www.st.com/resource/en/application_note/an5130-lsm6ds3trc-alwayson-3d-accelerometer-and-3d-gyroscope-stmicroelectronics.pdf)
- [SPA06-003 数据手册](https://cdn-shop.adafruit.com/product-files/6420/spa06-003.PDF)
- [Adafruit SPA06-003 驱动（交叉核对系数位域和公式）](https://github.com/adafruit/Adafruit_SPA06_003)
- [ST 官方平台无关驱动](https://github.com/STMicroelectronics/lsm6ds3tr-c-pid)
- [ST 官方轮询例程](https://github.com/STMicroelectronics/STMems_Standard_C_drivers/blob/master/lsm6ds3tr-c_STdC/examples/lsm6ds3tr_c_read_data_polling.c)
- [PX4 SPA06-003 驱动](https://github.com/PX4/PX4-Autopilot/tree/main/src/drivers/barometer/goertek/spa06)

## 首次实物反馈与诊断修正

首次上板日志：IMU WHO_AM_I=0x00，气压计 0x76/0x77 均未发现，不能认定传感器正常。
此前 `run_self_check()` 没有纳入 IMU/气压计，导致传感器缺失时仍进入 `READY_LOCKED`；
本次已修正。主机测试和构建通过只证明软件在相应测试范围内通过，不代表实物通信通过。

对照 ST 官方轮询例程，SPI 读取流程为 CS 拉低、发送 `reg | 0x80`、接收数据、CS 拉高。
这里由 ESP-IDF 的 8-bit command phase 与硬件 CS 完成等价事务；没有额外 dummy byte。
补齐官方例程的上电等待（官方 15 ms，采用 20 ms），ID 最多读取三次，并保留每次传输状态。
现有手写寄存器驱动未直接引入 ST 整套库；复位、BDU、ODR、量程和数据量纲均按官方资料核对。

未找到可核实为 Goermicro/Goertek 原厂发布的 SPA06-003 GitHub 例程。
PX4 是飞控项目实现，Adafruit 是模块厂商实现，都不能称为芯片原厂例程；其 I²C
读寄存器流程与这里的 transmit/receive 一致。补偿继续以原厂手册四阶公式为准。

新日志输出 GPIO、SPI ID 尝试、I²C 各地址的 NACK/超时和空闲线电平；若两个预期地址
均 NACK，再扫描其他非保留地址，只报告应答、不自动把未知器件作为气压计。
若总线超时，立即保留错误码并停止扫描，避免把线路故障误报为不存在。
这些改动提供诊断信息，目前未证实能解决首次实物通信失败。

用户反馈当前仅 USB 供电，待实测 U3 VDD/VDDIO、U5 VDD/VDDIO 对地电压。
如均有正常 3.3 V，继续检查 SCL/SDA 空闲高电平及 IMU 的 CS/SCK/MOSI/MISO 波形、
实际连线、芯片型号与焊接；WHO_AM_I=0x00 本身不足以区分这些原因。
