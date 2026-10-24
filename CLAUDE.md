# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

BFCopilot 是仿生蝴蝶扑翼飞控固件，C 语言，**ESP-IDF v6.0.2**，目标 `esp32c6`（见 `sdkconfig.defaults`）。

仓库正在做结构重构：当前已恢复 `src/app`（板级自检用 bring-up app）、`src/bsp`、`src/lib`
和 `src/service`（目前只有 `melody` 乐谱播放）；`src/driver` 尚未恢复。根 `CMakeLists.txt`
只登记实际存在的层，因此工程可以直接配置和构建；`src/app` 存在时会把它设为唯一的构建目标。

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

`build/` 是 gitignore 的构建目录，clangd 通过 `.vscode/settings.json` 使用其中的
`compile_commands.json`。整机 `idf.py build` 会同时编译 BSP、lib 和 app。

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
| `adc/` | 电池电压、舵机电流 | ADC1 连续采样（DMA）+ 逐通道曲线校准 + 芯片内置 IIR 滤波；读接口返回最近一个样本，只给 raw / mV，分压比和采样电阻不在 BSP 里换算 |
| `baro/` | SPA06-003 气压计 | I2C 100 kHz；0x76/0x77 双地址探测并用 ID（0x0D=0x11）校验；`spa06_math.c` 是纯计算补偿 |
| `buzzer/` | 蜂鸣器 | LEDC 10 bit，初始化后 0 占空比静音 |
| `com/` | 链路 | `bsp_wifi`：STA，凭据来自 Kconfig，默认关闭，`WIFI_STORAGE_RAM` 不存口令；`bsp_net`：多端口 TCP/UDP，单任务调度发送，TCP 每会话 StreamBuffer，UDP 每端口 MessageBuffer；`bsp_uart`：默认 UART0 原始收发 |
| `imu/` | LSM6DS3TR-C | `vendor/` 是 ST 官方 PID 2.2.3，**禁止修改**；SPI 用 `command_bits=8` 让 CS 覆盖地址+数据；固定 ±2 g / ±250 dps |
| `servo/` | 双路舵机 | MCPWM 50 Hz、1 µs 分辨率；上电输出强制低、舵机电源默认关断 |

### Wi-Fi 配置（`com/`）

凭据走 Kconfig，不写进代码：

```bash
idf.py menuconfig    # BFCopilot BSP → Enable Wi-Fi station / SSID / password
```

对应选项：`CONFIG_BF_WIFI_ENABLED`（默认 `n`）、`CONFIG_BF_WIFI_SSID`、
`CONFIG_BF_WIFI_PASSWORD`、`CONFIG_BF_WIFI_CONNECT_TIMEOUT_MS`。
真实口令只写进 `sdkconfig`（已 gitignore）或本地的 `local.defaults`，
**不要提交到 `sdkconfig.defaults`**。运行时 `bsp_wifi_init()` → `bsp_wifi_connect(timeout_ms)`，
断线会自动重连，`bsp_wifi_disconnect()` 停止重连。Wi-Fi + lwIP 需要 4 MB flash 和
large app 分区，这两项已写进 `sdkconfig.defaults`。

### 收发链路（`com/`）

拿到 IP 后可以直接开数据通道，不做业务帧格式。TCP 是可靠有序的字节流，UDP 是独立数据报，
由上层按用途选择；底层不跨协议群发。三个模块的头文件统一由
`bsp_com.h` 汇总，使用方只 include 它：

```c
const bsp_net_config_t ports[] = {
    {.mode = BSP_NET_TCP_SERVER, .local_port = 3333},
    {.mode = BSP_NET_TCP_SERVER, .local_port = 3335},
    {.mode = BSP_NET_UDP, .local_port = 3334},
    {.mode = BSP_NET_UDP, .local_port = 3336},
};
ESP_ERROR_CHECK(bsp_net_start(ports, sizeof(ports) / sizeof(ports[0])));

bsp_net_peer_t peers[BSP_NET_MAX_TCP_CONNECTIONS];
size_t count;
ESP_ERROR_CHECK(bsp_net_tcp_peers(0, peers, BSP_NET_MAX_TCP_CONNECTIONS, &count));
if (count != 0) {
    size_t received;
    bsp_net_tcp_recv(&peers[0], buf, sizeof(buf), &received, 100);
    bsp_net_send(&peers[0], buf, len, 100);
}

bsp_net_peer_t target = {.endpoint = 2, .ip = "192.168.137.1", .port = 9000};
bsp_net_send(&target, buf, len, 0);
bsp_net_datagram_t packet;
if (bsp_net_udp_recv(2, &packet, 100) == ESP_OK) {
    bsp_net_send(&packet.peer, packet.data, packet.len, 0);
}

bsp_uart_init(NULL);                                // 默认 UART0 / 115200
bsp_uart_send(buf, len);
int n = bsp_uart_recv(buf, sizeof(buf), 100);
```

配置数组下标就是 `endpoint`，最多 `BSP_NET_MAX_ENDPOINTS`（默认 4）个端口，可混合
TCP 服务端、TCP 客户端、UDP。每端口独立收发；同协议不能重复绑定相同的非零本地端口，
TCP 与 UDP 可以使用相同端口号。客户端配置 `remote_host/remote_port`，`local_port=0`
由协议栈分配本地端口，连接失败/断线自动重连。TCP 所有端口共享
`BSP_NET_MAX_TCP_CONNECTIONS`（默认 4）个会话，客户端各预留一个，其余供服务端使用。

`bsp_net_send()` 复制数据进指定连接/UDP 端口的独立 Queue，由一个网络任务轮询调度；
`ESP_OK` 仅表示入队，缓冲可立即复用，不表示已发送或远端确认。`timeout_ms` 是等待队列空间
的时间，满且超时返回 `ESP_ERR_TIMEOUT`。TCP 短写保留偏移继续发送，队首请求超过
`BSP_NET_TCP_SEND_TIMEOUT_MS`（默认 200 ms）仍未发完则断开，丢弃该会话全部待发请求，
重连不重放。慢连接不阻塞其他端口。UDP 每包只尝试发送一次，拥塞丢弃，不做应用层重传。

TCP 每会话使用独立 StreamBuffer，只保留字节顺序，不保留业务帧边界；满时暂停读取 socket
产生背压。`bsp_net_tcp_peers(endpoint, ...)` 查询连接快照，包括断线但仍有未读尾部的会话；
尾部读完才允许复用槽位。`connection_id` 每次连接更新，跨 stop/start 也不复用旧会话标识。
`bsp_net_tcp_recv()` 通过 `received` 返回字节数，暂无数据返回 `ESP_ERR_TIMEOUT`，
失效/停止返回 `ESP_ERR_INVALID_STATE`。一个会话的并发读取串行化，不混入其他连接。

UDP 每端口使用独立 MessageBuffer，一次取出完整 `bsp_net_datagram_t`，保留来源及所属
`endpoint`，支持空数据报。超长报文或容器满时整包丢弃。单次收发上限均为
`BSP_NET_MAX_PAYLOAD`（默认 1024 字节）。直接将 `packet.peer` 传给统一发送接口即可从
原本地端口回复实际发送方；主动发送须显式指定端口和远端地址，没有默认/最近对端或接收回调。
`bsp_net_stop()` 取消等待、丢弃待发请求并释放 socket 和容器；再次 start 可使用新的配置。

UART0 同时是 console，日志和链路数据共线；需要干净链路时把 console 切到
USB Serial JTAG。`CONFIG_BF_WIFI_ENABLED=n` 时网络状态为未运行，stop 为空操作，
其余网络接口返回 `ESP_ERR_NOT_SUPPORTED`。

### `src/app/` —— 板级自检入口

`app_main.c` 只做 bring-up：按顺序初始化 servo / buzzer / adc / imu / baro（Wi-Fi 按 Kconfig），
然后每 100 ms 采样打印。**不包含飞行逻辑，也不会给舵机上电**。`src/driver`、`src/service`
恢复后，app 仍是唯一入口，负责把各层串起来。

### `src/service/` —— 业务服务

- `melody/`：乐谱播放。`melody_note_t` 只写音名、八度和时长，频率在运行时按十二平均律
  从 MIDI 编号算出并夹到 BSP 音域；`melody_play()` 是阻塞接口，调用方应放到独立 task。
  `melody_songs.c` 内置几首公有领域曲子（`melody_songs[]`），bring-up app 上电顺序放一遍。

### 规划中的层

`src/driver` 还没恢复。恢复后保持依赖方向 **app → service → driver → bsp/lib**，
不要把业务逻辑塞进 BSP。

## 代码约定

- 风格沿用 `CNC_Power_Supply`：`u8/u16/u32/f32`、中文注释解释“为什么”。
- **BSP 接口统一用 `esp_err_t`**：`bsp_*` 直接返回 ESP-IDF 错误码，不做 `exit_code_t` 转换层；
  参数错误 `ESP_ERR_INVALID_ARG`、未初始化/重复初始化 `ESP_ERR_INVALID_STATE`、
  「本轮暂无新数据」`ESP_ERR_NOT_FINISHED`。`exit_code_t` 只留在 `src/lib`。
- **不要新建 `xxx_config.h`**：引脚和参数宏直接放进模块的 `bsp_xxx.h`（用户明确要求）。
- 每个顶层目录是一个 ESP-IDF 组件，用 `idf_component_register` 注册；include 写成
  `"bsp_adc.h"` 或 `"lib/tools/common_def.h"`。
- 公共头不要暴露驱动类型（例如 `I2C_NUM_0` 只放在 `.c` 里），避免使用方被迫依赖 ESP-IDF 驱动头；
  `esp_err.h` 这类基础类型头可以出现在公共头里。

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
