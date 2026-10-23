# 遥控上位机（`tools/bf_rc.py`）

默认启动新版浏览器可视化工作台：Python 继续读取 USB 摇杆、维持 UDP 会话，
浏览器显示与操作界面。无需 Node.js、npm 构建、Tcl/Tk 或互联网，也不使用浏览器的
Gamepad API。网页服务仅监听 `127.0.0.1`。

- 遥控器图实时显示左右摇杆与 A4–A7 辅助通道；标注可点击查看映射。
- B0–B15 按 USB 上报编号显示按下状态，可分配解锁和急停；实体电源、绑定键
  不假定会通过 USB 上报。辅助通道的实体开关位置需要拨动实物核对。
- 舵机图显示下位机输出指令，另有电压、电流、故障、遥测年龄、控制帧年龄、
  剩余租约、收发/坏帧/拒绝计数，以及最近 30 秒的摇杆、舵机、电源曲线。
- 参数抽屉提供轴映射、反向、油门端点校准、命令滑块和本地限幅。飞控实际参数
  不会通过当前 UDP 协议回传；本地限幅不代表已经读到飞控参数。
- 未连接或过期的遥测显示 `—`，不以 0 或演示值代替；舵机指令不代表位置实测。

## 1. 拓扑

```text
BetaFPV 小白控三代（LiteRadio 3）
        │ USB HID（8 轴 / 16 键）
        ▼
     电脑 ── Wi-Fi（与飞控同一热点）── UDP:3335 ──▶ 飞控 ESP32-C6
        ◀──────────── STATUS / ACK ─────────────────┘
```

- 这是**独立的遥控上位机**，不复用台架调试站 `tools/bf_station.py`，也不连接台架固件。
- 电脑侧只做三件事：读 USB 摇杆、把摇杆映射成飞控的 60 字节 CONTROL 帧、维持会话/lease/ACK。
- 飞控侧由 `flight` 服务继续做 session、序号、TTL、lease、ARM 前置条件和安全状态机；Wi-Fi 只替换传输层，不绕过任何校验。

## 2. 运行

```bash
uv run --project tools python tools/bf_rc.py
uv run --project tools python tools/bf_rc.py --list                 # 只列出摇杆
uv run --project tools python tools/bf_rc.py --host 192.168.x.x --port 3335
uv run --project tools python tools/bf_rc.py --no-joystick          # 只用界面滑块
uv run --project tools python tools/bf_rc.py --demo                 # 模拟数据预览，不操作设备
uv run --project tools python tools/bf_rc.py --no-browser           # 输出地址，手动打开
uv run --project tools python tools/bf_rc.py --web-port 8765        # 指定本机网页端口
uv run --project tools python tools/bf_rc.py --legacy               # 旧版 Tk 界面
```

新版默认选择空闲网页端口并打开浏览器；请使用终端打印的完整地址，地址中的 `#` 后面
是本次启动的访问凭据。按 `Ctrl+C` 停止服务。浏览器关闭时请求急停，页面心跳中断
超过 1.5 秒时本机服务也会请求急停；恢复后需显式重新连接。多页面同时打开时，
只有取得控制权的页面可以更改控制，其他页面只读，但仍允许急停。
解锁由服务端验证连续按住至少 1.2 秒，松开、移出按钮、切走窗口或中断请求会取消。

演示模式会明确标注模拟数据，不创建 USB/UDP 采集线程，不允许设备操作或保存配置。
退出演示、重新正常启动即可使用真实摇杆与飞控遥测。

依赖 `pygame-ce`（`tools/pyproject.toml` 已声明，`uv` 会自动安装）。首次启动会读取
`~/.bfcopilot/rc.json`；界面里的“保存到本机”会写回该文件。

前置条件：

1. 飞控固件启用 `CONFIG_BF_WIFI_LINK_ENABLED=y`；默认 `CONFIG_BF_WIFI_LINK_MODE_STA=y`，
   在 `wifi.local.defaults` 里填上游 SSID/密码；
2. 电脑的 Wi-Fi 连到**同一个热点**（飞控不再自己开热点；需要 AP 时选
   `CONFIG_BF_WIFI_LINK_MODE_AP=y`，电脑连 `BFCopilot`）；
3. 飞控 IP 由路由器分配，串口日志会打印 `joined "<SSID>": 192.168.x.x`；
   `bf_rc.py` 默认广播发现，也可以 `--host` 直接指定。UDP 端口默认 `3335`。

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

### 已识别摇杆，但界面轴值不变化

摇杆采集使用 SDL 隐藏窗口，Tk 界面获得焦点时该窗口处于后台。采集线程会在初始化
pygame 前设置 `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`，使轴值和按键持续更新。
如果运行的是修复前已打开的上位机，需要关闭窗口并重新启动；仅“刷新摇杆”不会
重新初始化 SDL。检查轴值不需要连接飞控或解锁。

## 5. 控制语义

| 飞控模式 | 上位机行为 |
|---|---|
| 0 保持 | 未解锁时强制使用；中心角来自俯仰/横滚，频率/幅度/相位/差动全 0 |
| 1 手动 | 解锁后可用；静态角度，映射同保持 |
| 2 扑翼 | 只在解锁后可用；油门 → 频率，俯仰 → 中心，横滚 → 差动，偏航 → 相位 |

- 使用摇杆时，油门在最低位（`<= 0.05`）不会起扑；切到扑翼后需要推油门。
  关闭摇杆输入后，扑翼频率由界面滑块直接控制。
- 本地限幅默认与参数默认值一致（最大频率 8 Hz、速度 857 °/s、加速度 43000 °/s²、
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
- 显式启用台架 Wi-Fi 时限配置时，TTL 为 300 ms、lease/遥测超时为 500 ms；
  网页显示当前配置。仅限有人看护的台架测试，急停仍优先处理，不自动重连或解锁。
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

默认配置（STA 加入现有热点）：

```text
CONFIG_BF_WIFI_LINK_ENABLED=y
CONFIG_BF_WIFI_LINK_MODE_STA=y
CONFIG_BF_WIFI_STA_SSID="TX5PRO"
CONFIG_BF_WIFI_STA_PASSWORD="..."     # 8..63 字符；空 = 开放热点，仅台架
CONFIG_BF_WIFI_LINK_PORT=3335
```

热点跨频段调度造成抖动时，可在本地构建配置显式加入
`CONFIG_BF_WIFI_LINK_BENCH_TIMING=y` 后重新构建并烧录。此选项默认关闭；启用后
CONTROL TTL 上限为 300 ms，lease 为 500 ms。上位机根据 HELLO 首次 STATUS
自动识别，并显示“台架 Wi-Fi”提示，遥测超时同步为 500 ms。每次连接重新协商。
这会延后失联保护，仅用于台架；关闭选项恢复严格的 100/150 ms 配置。

需要飞控自己开热点时改用 `CONFIG_BF_WIFI_LINK_MODE_AP=y` 加
`CONFIG_BF_WIFI_AP_SSID/PASSWORD/CHANNEL`，地址固定为 `192.168.4.1`。

## 8. 协议

- 每个 UDP 数据报恰好一个 60 字节帧，不做 COBS；帧格式见 `docs/protocol.md`。
- 飞控监听 `CONFIG_BF_WIFI_LINK_PORT`，把 STATUS/ACK 回复到最近一次有效数据报的
  源地址/端口；电脑绑定临时本地端口即可。
- 发现：电脑广播 ASCII `BF-RC-DISCOVER`，飞控回复 `BF-RC-HERE`；只用于找 IP。
- 控制 50 Hz，STATUS 50 Hz，CONTROL TTL 100 ms，lease 150 ms。
- CONTROL 的 `upstream_age_ms` 由上位机置 0；从电脑到飞控的延迟必须小于 TTL。

### 握手成功后遥测超时

“会话建立”说明已收到飞控的 STATUS；之后提示“150 ms 未收到 STATUS”，是从最后一帧
有效 STATUS 开始计时，并不是从 HELLO 开始计时。飞控在 CONTROL 的 TTL 或 lease
过期后结束会话并停止发送 STATUS，因此这条提示也可能由控制帧中断引起。

Wi-Fi 驱动初始化时显式设置 `WIFI_PS_NONE`，关闭默认的 Modem-sleep，避免 STA
接收控制帧时等待热点的 DTIM 周期；修改后需要重新构建并烧录飞控才能生效。
此设置会增加耗电；时限由上述严格/台架配置单独决定。

如果仍然断开，查看同一时刻的飞控串口日志：`control deadline missed` 表示控制任务
超期；`control TTL or lease expired` 表示解锁状态下控制帧或 lease 过期；
`wifi_link: lost` 表示飞控掉出热点。锁定状态下 TTL 过期也会结束会话，但不会报
`LINK_LOST` 故障。仅凭上位机这条超时提示无法区分这些原因。

## 9. 验证

```bash
uv run --project tools python -m unittest discover -s test -p "test_rc_*.py"
```

`test_rc_web.py` 覆盖 HTTP 访问凭据、跨来源请求、只读观察窗口、连续按住解锁、
断开浏览器急停、配置校验和模拟模式隔离；本地假飞控验证连接、解锁、扑翼与急停。

2026-10-23 在 `192.168.137.158` 实板验证：烧录主工程 BFCopilot 飞控固件
（不是 `test/bench`），启用上述 Wi-Fi 时限配置后，零运动控制运行 60 秒无掉线，
ARM/DISARM 均成功；真实网页长按解锁后持续 20 秒保持 ARMED，无故障。
另行验证停发 CONTROL 触发 `LINK_LOST` 回锁、ESTOP 结束会话并锁存；控制任务
超期计数为 0。严格和台架时限分别运行原生 flight/safety 回归，覆盖 TTL 上限、
到期边界、未到期旧 lease 的保留和跨会话旧 lease 拒绝。此记录不代表飞行验收。
网页已用 Edge/Playwright 检查实时动画、映射/参数抽屉、曲线切换、1440/1024/390 像素
布局及控制台错误。

## 10. 已知限制

- **没有应用层认证。** WPA2 热点是唯一隔离；同一热点上的客户端可以伪造帧。
  这条链路只用于台架/室内调试，不作为可飞行链路。生产部署需要配对和消息认证。
- 没有 RSSI、丢包率、参数读写、实际舵机位置反馈；STATUS 里的舵机值是输出指令。
- 摇杆轴映射/油门校准需要按实物确认；默认值来自当前这台 BetaFPV 的实测轴顺序。
- 飞控固件与实物链路尚未在本次改动中构建/烧录验证（构建由用户另行执行）。
