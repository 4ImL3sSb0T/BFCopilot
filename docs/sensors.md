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

- IMU：104 Hz，±2 g、±250 °/s；软件复位、带超时等待、BDU、地址递增、配置回读
  （均由 ST 驱动的 `reset_set` / `block_data_update_set` / `auto_increment_set` /
  `*_data_rate_set` / `*_full_scale_set` 与对应 getter 完成）。读 `STATUS_REG` 确认
  加速度与角速度均 data-ready 后，分两次各读 6 字节原始值，输出加速度 mg、角速度 mdps
  （毫度/秒）。
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

纯 C 主机测试覆盖有符号位域、完整系数解包、正负原始值及 c31/c40 补偿、无效系数和
超范围结果。驱动模拟测试覆盖实际驱动代码的芯片 ID 拒绝、复位/系数等待超时、0x76 地址
回退、IMU 配置寄存器写入与回读、已知原始值经 ST 换算函数的量纲结果、温压异步就绪、
样本过期、I/O 失败与下一次正常采样恢复。

```powershell
cmake -S test/host -B test/host/build
cmake --build test/host/build
ctest --test-dir test/host/build --output-on-failure
scripts\idf.cmd build
```

本次验证状态为 `offline-verified`：四组 CTest 全部通过，ESP-IDF v6.0.2 的 ESP32-C6
完整构建通过，生成 `BFCopilot.bin` 为 390000 字节。实物传感器通信仍未通过，见下方
实物反馈记录；主机测试与构建通过只代表软件在相应范围内正确。

`sensors.c` / `motion.c` / ST 驱动未纳入主机测试的部分（真实 SPI 时序、波形）只能上板
验证。烧录后先看两条初始化成功日志，再在 `bf>` 输入 `status`：静止加速度向量长度应约
1000 mg、角速度接近零，气压应在 30000–110000 Pa 范围。翻转板子检查各轴符号，断开传感器
检查有效标志失效。实物采样、安装方向、噪声与零偏仍需台架验证。

## 驱动来源与维护

### IMU：使用 ST 官方驱动

LSM6DS3TR-C 不再使用手写寄存器代码，改为收录 ST 官方平台无关驱动（PID）。

| 项 | 值 |
|---|---|
| 上游 | https://github.com/STMicroelectronics/lsm6ds3tr-c-pid |
| 版本 | Release version 2.2.3 |
| 提交 | `6735cd1f6ea9208084f2e7dc69605250bc532ac6`（2026-10-08） |
| 许可 | BSD-3-Clause |
| 位置 | `components/lsm6ds3tr_c/vendor/`，逐字收录，不修改 |

总线对接放在 `components/sensors/motion.c`：实现 `platform_read` / `platform_write` /
`platform_delay` 三个回调并填入 `stmdev_ctx_t`。回调返回 0 表示成功，失败时原样回传
`esp_err_t`，因此 `motion.c` 能把驱动返回值直接还原成精确错误码（例如 SPI 超时仍报
`ESP_ERR_TIMEOUT`）。配置与读取全部走 ST 的语义接口
（`device_id_get` / `reset_set` / `block_data_update_set` / `auto_increment_set` /
`xl_data_rate_set` / `gy_data_rate_set` / `xl_full_scale_set` / `gy_full_scale_set` /
`status_reg_get` / `acceleration_raw_get` / `angular_rate_raw_get` /
`from_fs2g_to_mg` / `from_fs250dps_to_mdps`），`motion.c` 里不再出现裸寄存器地址或位域。

**SPI 事务结构必须注意**：SPI 读要置地址最高位（`reg | 0x80`），且 ST 的参考例程靠手动
GPIO 拉低 CS 横跨"发地址"和"收数据"两次调用。ESP-IDF 的硬件 CS 每次事务结束都会释放，
照搬会破坏读时序，所以这里用 `command_bits = 8`：地址进 command phase、数据进同一事务的
数据阶段，一次调用内 CS 保持有效。这与原来手写驱动的事务结构一致。

量纲换算因此改由 ST 的 `from_*` 函数给出（返回 `float_t`，用 `lroundf` 取整后存入整型字段），
与原先 `accel * 61 / 1000`、`gyro * 875 / 100` 的整数运算结果一致；原 `motion_math.c` 里的
`lsm6ds3tr_decode` 已随之删除，换算覆盖改由驱动测试通过 ST 真实代码验证。

### 气压计：无原厂驱动可用

SPA06-003 没有找到可核实为 Goermicro/Goertek 原厂发布、且能直接用于本工程的驱动：

- ESP 组件仓库搜 `spa06`、`goertek` 均为空；仅有一个私人上传的 `ningzixi/spl06 0.1.0`
  （0 下载、无仓库地址），不足以作为依据。
- GitHub 上只有两个第三方实现：[Adafruit_SPA06_003](https://github.com/adafruit/Adafruit_SPA06_003)
  是模块厂商实现且依赖 Arduino `Wire` / `Adafruit_BusIO`；
  [PX4 spa06](https://github.com/PX4/PX4-Autopilot/tree/main/src/drivers/barometer/goertek/spa06)
  是飞控项目实现，C++ 且绑定 PX4 的 `device::I2C` 与任务模型。两者都不是原厂例程，
  移植成本高于收益。

因此气压计**保留按原厂手册实现**的 `motion_math.c`（系数解包 + 四阶温压补偿，已与
Adafruit 实现交叉核对，并由主机测试覆盖）。若后续找到原厂驱动，再按 IMU 的方式收录。

## 寄存器参考

- [ST LSM6DS3TR-C 数据手册](https://www.st.com/resource/en/datasheet/lsm6ds3tr-c.pdf)
- [ST AN5130](https://www.st.com/resource/en/application_note/an5130-lsm6ds3trc-alwayson-3d-accelerometer-and-3d-gyroscope-stmicroelectronics.pdf)
- [SPA06-003 数据手册](https://cdn-shop.adafruit.com/product-files/6420/spa06-003.PDF)
- [Adafruit SPA06-003 驱动（交叉核对系数位域和公式）](https://github.com/adafruit/Adafruit_SPA06_003)
- [ST 官方平台无关驱动](https://github.com/STMicroelectronics/lsm6ds3tr-c-pid)
- [ST 官方轮询例程](https://github.com/STMicroelectronics/STMems_Standard_C_drivers/blob/master/lsm6ds3tr-c_STdC/examples/lsm6ds3tr_c_read_data_polling.c)
- [PX4 SPA06-003 驱动](https://github.com/PX4/PX4-Autopilot/tree/main/src/drivers/barometer/goertek/spa06)

## 实物反馈记录

### 2026-10-13：改用纯硬件测试固件后复测（I²C 误差不稳定）

台架固件重做为不跑飞控栈的纯硬件测试固件（见 `docs/PLAN.md` 实施记录第二版）后复测，现象如下：

```
$ hw scan
spi2     : SCK=7 MOSI=15 MISO=14 CS=6, MISO idle=0
imu      : WHO_AM_I=0x00 (expected 0x6a), transfer=ESP_OK
i2c0     : SCL=4 SDA=5, idle SCL=1 SDA=1
i2c ack  : 0 address(es)
            probe reported ESP_ERR_TIMEOUT - a bus fault, not a missing device
$ hw baro
0x77     : no ACK (ESP_ERR_NOT_FOUND)
0x76     : no ACK (ESP_ERR_NOT_FOUND)
```

新增判读（此前没有的关键信息）：

- **同一条总线、同一组地址，两个命令给出不同错误码**：全地址扫描（0x08–0x77）报
  `ESP_ERR_TIMEOUT`，定点探测 0x77/0x76 报 `ESP_ERR_NOT_FOUND`（NACK）；连续三次扫描结果一致。
  这说明不是"干脆地没有器件"，而是**总线处于临界状态**——与共用 3V3 缺失（外部上拉失效、只剩
  内建弱上拉勉强把线拉高、边沿过慢导致超时）相符。
- IMU 侧不变：`transfer=ESP_OK`（时钟确实打出去了）、`MISO idle=0`、`WHO_AM_I=0x00`；
  `hw imu stream` 10 Hz 采样全为 0。
- `hw adc` 两路原始计数 ≈0（本次未接电池组，属预期）。
- 待办优先级不变，且更应优先：先实测 U3/U5 的 VDD/VDDIO，**再确认 R8/R9 上拉所接的 3V3 轨是否
  带电**，然后才查 SDO/SCL/SDA 连通性。

### 2026-10-13：补焊后用台架固件的 `hw` 命令复测（现象不变）

用台架固件（`test/bench`，见 `docs/PLAN.md` 实施记录）在无线条件下逐器件复测，不再依赖串口：

```
$ hw scan
spi2     : SCK=7 MOSI=15 MISO=14 CS=6, MISO idle=0
imu      : WHO_AM_I=0x00 (expected 0x6a), transfer=ESP_OK
i2c0     : SCL=4 SDA=5, idle SCL=1 SDA=1
i2c ack  : 0 address(es)
$ hw imu
WHO_AM_I : 0x00 (expected 0x6a), transfer=ESP_OK
STATUS   : 0x00 (xlda=0 gda=0), transfer=ESP_OK
accel raw: [0, 0, 0] LSB
gyro raw : [0, 0, 0] LSB
$ hw adc
vbat     : GPIO0 raw=0 pin=0 mV -> 0 mV (invalid)
iservo   : GPIO1 raw=1 pin=1 mV -> 0 mA (invalid)
```

判读：

- 与 10-11、10-13 两次结论一致：**SPI 事务完成（`transfer=ESP_OK`）、I²C 空闲电平正常
  （SCL/SDA=1），但两片传感器都不应答**。补焊后的现状与补焊前没有区别。
- `hw scan` 对 0x08–0x77 全地址探测，**0 个 ACK**，且探测返回的是 `ESP_ERR_NOT_FOUND`
  （NACK）而不是超时——说明是"总线上没人应答"，不是总线被拉死；这与上拉正常的空闲电平一致。
  `hw scan` 会区分这两者，避免把线路/供电故障误读成器件缺失。
- MISO 恒为 0 仍指向 IMU 未上电或 SDO 未连通。
- 待办不变，优先级不变：先实测 U3/U5 的 VDD/VDDIO 是否为 3.3 V 及共地，再查 SDO/SCL/SDA
  连通性；`hw imu rd <reg>` / `hw baro rd <reg> [addr]` 可在不断电的情况下逐个寄存器复核。
- `hw adc` 两路原始计数≈0：本次测量未接电池组，属预期；接上电池组后应能在 `hw adc` 看到与
  万用表一致的引脚电压。
- 本次启动的 `events` 只有 `IMU_LOST`/`BARO_LOST`/`PARAM_CORRUPT` 三条预期项，没有 `TASK_WDT`。

### 2026-10-11：首次上板（手写驱动）

IMU WHO_AM_I=0x00，气压计 0x76/0x77 均未发现，不能认定传感器正常。
此前 `run_self_check()` 没有纳入 IMU/气压计，导致传感器缺失时仍进入 `READY_LOCKED`；
本次已修正。主机测试和构建通过只证明软件在相应测试范围内通过，不代表实物通信通过。

对照 ST 官方轮询例程，SPI 读取流程为 CS 拉低、发送 `reg | 0x80`、接收数据、CS 拉高；
这里由 ESP-IDF 的 8-bit command phase 与硬件 CS 完成等价事务，没有额外 dummy byte。
补齐官方例程的上电等待（官方 15 ms，采用 20 ms），ID 最多读取三次，并保留每次传输状态。

日志输出 GPIO、SPI ID 尝试、I²C 各地址的 NACK/超时和空闲线电平；若两个预期地址
均 NACK，再扫描其他非保留地址，只报告应答、不自动把未知器件作为气压计。
若总线超时，立即保留错误码并停止扫描，避免把线路故障误报为不存在。

### 2026-10-13：换上 ST 官方驱动后复测（结果不变）

烧入含 ST 官方驱动 2.2.3 的固件（`build/BFCopilot.bin`，已确认二进制内含该驱动），
现象与首次完全相同：

```
W motion: IMU identity attempt 1: transfer=ESP_OK WHO_AM_I=0x00 MISO=0
W motion: IMU identity attempt 2: transfer=ESP_OK WHO_AM_I=0x00 MISO=0
W motion: IMU identity attempt 3: transfer=ESP_OK WHO_AM_I=0x00 MISO=0
W motion: LSM6DS3TR-C WHO_AM_I=0x00, expected 0x6a
W motion: IMU unavailable: ESP_ERR_NOT_FOUND
I motion: I2C probe 0x77: ESP_ERR_NOT_FOUND, SCL=1 SDA=1
I motion: I2C probe 0x76: ESP_ERR_NOT_FOUND, SCL=1 SDA=1
W motion: No SPA06-003 at 0x76/0x77; 0 other I2C addresses ACK
```

判读：

- `transfer=ESP_OK` 说明 ESP32 侧 SPI 事务本身正常完成。SPI 无应答检测，该状态只证明
  主机把时钟和片选发出去了，不证明从设备在响应。
- 同一套总线操作下官方驱动读到的东西与手写驱动完全一致 ⇒ **排除"驱动实现错误"这一假设**。
- SPI 与 I²C 是两条独立总线，却同时"对面什么都没有"，而 SCL/SDA 空闲电平正常为高
  （上拉与走线健康）⇒ 指向两片传感器**共用的供电**未到位，或两片各自焊接/连线问题。
  最可能是一次性能解释两者同时失效的原因：U3 与 U5 的 VDD/VDDIO 无 3.3 V。
- MISO 空闲恒为 0 与 `WHO_AM_I=0x00` 一致，说明该线没有被有效驱动（芯片未上电、SDO 短地、
  或 SDO 未连通）；但若该网络本就没有上拉，高阻悬空的读数本身不足以单独定性。

待办（按优先级）：先实测 U3 VDD/VDDIO、U5 VDD/VDDIO 对地电压及共地；供电正常再用示波器
抓 GPIO6/7/15 在身份读取期间是否有 1 MHz 时钟、CS 拉低和 `0x8F` 读地址；再量 MISO 到
U3 SDO 的连通性与是否短地；最后核对 U5 的 CSB/SDO 实际电平与连线。
在此之前，`IMU_LOST` / `BARO_LOST` 与 `FAULT_LATCHED` 是预期的安全行为，不是缺陷。

