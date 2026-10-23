# 遥控上位机（`tools/bf_rc.py`）

## 1. 拓扑

```text
BetaFPV 小白控三代（LiteRadio 3）
        │ USB HID（8 轴 / 16 键）
        ▼
     电脑 ── Wi-Fi（电脑连到飞控热点）── UDP:3335 ──▶ 飞控 ESP32-C6
        ◀──────────── STATUS / ACK ─────────────────┘
```

- 这是**独立的遥控上位机**，不复用台架调试站 `tools/bf_station.py`，也不连接台架固件。
- 电脑侧只做三件事：读 USB 摇杆、把摇杆映射成飞控的 60 字节 CONTROL 帧、维持会话/lease/ACK。
- 飞控侧由 `flight` 服务继续做 session、序号、TTL、lease、ARM 前置条件和安全状态机；Wi-Fi 只替换传输层，不绕过任何校验。

## 2. 运行

```bash
uv run --project tools python tools/bf_rc.py
uv run --project tools python tools/bf_rc.py --list                 # 只列出摇杆
uv run --project tools python tools/bf_rc.py --host 192.168.4.1 --port 3335
uv run --project tools python tools/bf_rc.py --no-joystick          # 只用界面滑块
```

依赖 `pygame-ce`（`tools/pyproject.toml` 已声明，`uv` 会自动安装）。首次启动会读取
`~/.bfcopilot/rc.json`；界面里的“保存到本机”会写回该文件。

前置条件：

1. 飞控固件启用 `CONFIG_BF_WIFI_LINK_ENABLED=y` 并配置 AP SSID/密码；
2. 电脑的 Wi-Fi 连到飞控热点（默认 SSID `BFCopilot`）；
3. 飞控 IP 默认 `192.168.4.1`，UDP 端口默认 `3335`。

## 3. 界面

| 区域 | 作用 |
|---|---|
| 链路 | IP/端口、连接、断开、扫描；会话 ID、序号、遥测年龄 |
| 飞控状态 | 安全状态、armed、故障列表 |
| 电源/输出 | 电池电压、舵机电流、左右指令、扑翼相位、控制帧年龄、lease、拒绝帧 |
| 模式 | 保持中位 / 手动角度 / 正弦扑翼；未解锁时不能切扑翼 |
| 摇杆 | 左杆油门/偏航、右杆俯仰/横滚；关闭“使用摇杆”后改用滑块 |
| 命令 | 当前要发的完整命令和本地收敛提示 |
| 操作 | 按住解锁（1.2 s）、上锁、清故障、急停（`Esc`） |
| 事件 | 会话、ACK、拒绝、故障和操作记录 |
| 摇杆映射 | 实时轴值、轴角色、反向、油门低位/高位校准、摇杆解锁/急停键 |
| 链路详情 | session/sequence/lease/ACK/收发计数 |
| 设置 | 死区、指数、中心/相位/差动范围、本地限幅 |
| 安全说明 | 链路边界和操作顺序 |

## 4. 摇杆映射

小白控三代在 USB HID 模式下报告 8 个轴、16 个按键，没有 hat。默认映射：

| 轴 | 角色 | 说明 |
|---|---|---|
| 2 | 油门 | 机械下限约为 `-0.83`，不是 `-1.0`；用“记录最低点/最高点”校准 |
| 0 | 偏航 | 扑翼模式 → 相位 |
| 1 | 俯仰 | 保持/手动 → 公共中心角；扑翼 → 公共中心角 |
| 3 | 横滚 | 保持/手动 → 左右中心角差；扑翼 → 差动 |
| 4–7 | 开关 | 当前未分配 |

- 死区默认 `0.04`，指数默认 `0.25`；两者都在“设置”里。
- 幅度不由摇杆决定：在“命令”里的左/右幅度滑块设置，映射时作为 `amplitude_deg`。
- 摇杆解锁键默认“无”。配置后必须**按住 1 秒**才发 ARM；松开重新计时，不会因为开关
  一直停在解锁位而自动解锁。
- 摇杆急停键默认“无”。配置后按下即连发 3 帧 ESTOP。

## 5. 控制语义

| 飞控模式 | 上位机行为 |
|---|---|
| 0 保持 | 未解锁时强制使用；中心角来自俯仰/横滚，频率/幅度/相位/差动全 0 |
| 1 手动 | 解锁后可用；静态角度，映射同保持 |
| 2 扑翼 | 只在解锁后可用；油门 → 频率，俯仰 → 中心，横滚 → 差动，偏航 → 相位 |

- 使用摇杆时，油门在最低位（`<= 0.05`）不会起扑；切到扑翼后需要推油门。
  关闭摇杆输入后，扑翼频率由界面滑块直接控制。
- 本地限幅默认与参数默认值一致（最大频率 1 Hz、速度 180 °/s、加速度 360 °/s²、
  行程 ±90°）。上位机先按协议的离散精度收敛频率、差动、中心角和幅度，
  再校验最终命令，避免编码舍入使飞控拒绝整帧。
- 这些本地限幅只是**估计**，飞控参数才是最终依据。飞控拒绝帧时 STATUS 的
  `rejected_frames` 会增加，界面会显示。
- 协议不支持读取飞控参数；改飞控参数仍需用飞控自己的 console。

## 6. 安全规则

- 上电/连接后永远处于锁定；ARM 必须由人明确按住按钮或按住已配置的摇杆键。
- 解锁前检查：会话有效、飞控 `READY_LOCKED`、无故障、油门归零、模式不是扑翼、
  摇杆已连接（可关闭该要求）。
- CONTROL TTL 为 100 ms；控制帧或 lease 到期时飞控自行失联锁定。上位机崩溃、
  Wi-Fi 断开由飞控处理；上位机在 150 ms 没收到 STATUS 时也会关闭本地会话。
- 使用摇杆输入时，USB 摇杆断开立即发送 ESTOP 并关闭会话；重新插入不会自动恢复控制，
  必须重新连接、清除故障并显式解锁。关闭摇杆输入时，摇杆断连不影响滑块控制。
- 扫描在独立线程运行，不暂停控制帧、遥测接收或急停处理。
- 急停连发 3 帧 ESTOP 并在飞控本地锁存；之后必须重新 HELLO、CLEAR、CONTROL、ARM。
- 关闭窗口或点击“断开”时，若已解锁会先发 DISARM；关闭窗口发 ESTOP。
- 未解锁时只发保持中位，不会因为界面停在扑翼模式而自动起扑。

## 7. 飞控侧构建

```bash
cp scripts/wifi.local.defaults.example wifi.local.defaults
# 编辑 wifi.local.defaults，填 8..63 字符的 WPA2 密码

scripts/idf.sh -B build-wifi \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;wifi.local.defaults" build
scripts/idf.sh -B build-wifi -p COM4 flash monitor
```

`wifi.local.defaults` 已被 Git 忽略。`CONFIG_BF_WIFI_LINK_ENABLED` 与
`CONFIG_BF_RADIO_ENABLED` 互斥；默认关闭，不影响现有构建。

默认配置：

```text
CONFIG_BF_WIFI_LINK_ENABLED=y
CONFIG_BF_WIFI_AP_SSID="BFCopilot"
CONFIG_BF_WIFI_AP_PASSWORD="..."      # 8..63 字符；空 = 开放热点，仅台架
CONFIG_BF_WIFI_AP_CHANNEL=6
CONFIG_BF_WIFI_LINK_PORT=3335
```

飞控作为 AP 时默认地址是 `192.168.4.1`；电脑连上后会拿到 `192.168.4.x`。

## 8. 协议

- 每个 UDP 数据报恰好一个 60 字节帧，不做 COBS；帧格式见 `docs/protocol.md`。
- 飞控监听 `CONFIG_BF_WIFI_LINK_PORT`，把 STATUS/ACK 回复到最近一次有效数据报的
  源地址/端口；电脑绑定临时本地端口即可。
- 发现：电脑广播 ASCII `BF-RC-DISCOVER`，飞控回复 `BF-RC-HERE`；只用于找 IP。
- 控制 50 Hz，STATUS 20 Hz，CONTROL TTL 100 ms，lease 150 ms。
- CONTROL 的 `upstream_age_ms` 由上位机置 0；从电脑到飞控的延迟必须小于 TTL。

## 9. 已知限制

- **没有应用层认证。** WPA2 热点是唯一隔离；同一热点上的客户端可以伪造帧。
  这条链路只用于台架/室内调试，不作为可飞行链路。生产部署需要配对和消息认证。
- 没有 RSSI、丢包率、参数读写、实际舵机位置反馈；STATUS 里的舵机值是输出指令。
- 摇杆轴映射/油门校准需要按实物确认；默认值来自当前这台 BetaFPV 的实测轴顺序。
- 飞控固件与实物链路尚未在本次改动中构建/烧录验证（构建由用户另行执行）。
