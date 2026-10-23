---
name: bf-console
description: 操作 BFCopilot 飞控或台架固件的 USB-Serial-JTAG 控制台，使用项目 bf_cli 前端查询状态、读写参数、校正电池分压，并处理 Windows COM 串口输入失灵。用户要求使用飞控控制台、param get/set、status 或排查 idf.py monitor 无法输入时使用。
---

# BFCopilot USB 控制台

在仓库根目录运行现有前端 `tools/bf_cli.py`。它支持交互输入和 Agent JSON 输出；不要重写串口脚本来代替它。
本项目使用 ESP-IDF v6.0.2、ESP32-C6，飞控 `esp_console` 走板载 USB-Serial-JTAG。Windows 本机验证端口为 COM4，Espressif VID 为 `0x303A`；实际操作前以枚举结果为准。

## 查询与交互

```powershell
uv run --project tools python tools/bf_cli.py --json --list
uv run --project tools python tools/bf_cli.py --serial COM4 --json "status"
uv run --project tools python tools/bf_cli.py --serial COM4 --json "param get sensors.vbat_div_num" "param get sensors.vbat_div_den"
uv run --project tools python tools/bf_cli.py --serial COM4
```

交互模式用 `:quit` 或 Ctrl-D 退出。Agent 必须传 `--json` 并提供完整命令，每条命令是一个独立的带引号参数。
指定 `--serial` 可避免自动枚举选错设备。打开前关闭占用同一端口的 monitor、终端和其他控制台进程。

单条命令 stdout 是一个 JSON 对象，多条命令是一个 JSON 数组：

```json
{"command":"param get sensors.vbat_div_den","ok":true,"output":"sensors.vbat_div_den = 10.000"}
```

结果字段为 `command`、`ok`、`output`；串口、校验或超时错误另有 `error`。
退出码 `0` 表示命令回复未检测到错误，`1` 表示固件返回错误，`2` 表示连接、输入或收发异常。
多条命令遇到收发异常会停止，数组只包含已尝试的命令；不能把未执行的后续命令当作成功。
`--json --list` 返回端口对象数组，字段为 `port`、`description`。

## 修改参数

按用户要求查询、修改，再读回核对；数值修改同时保存到 NVS。
`param set` 在舵机输出使能、ARMED、SAFE_RETURN_LOCKED 或有无线会话时会被拒绝。BOOT_LOCKED/READY_LOCKED 且输出关闭、无线会话关闭时可修改。
如果被拒绝，报告具体状态；不要为了完成参数写入自行解除安全锁、启用输出或改变无线会话。

VBAT 读数约为实际值两倍时，本板已验证的电池分压配置为 `27/10`：

```powershell
uv run --project tools python tools/bf_cli.py --serial COM4 --json "param set sensors.vbat_div_num 27" "param set sensors.vbat_div_den 10" "param get sensors.vbat_div_num" "param get sensors.vbat_div_den" "status"
```

2026-10-23 实板验证：读数从约 `8105 mV` 变为 `4050–4052 mV`，符合实测约 `4.05 V`。
这是本板案例，其他硬件应按实际电阻和测量结果确定比例。固件默认值更新不会覆盖 NVS 中已有参数。
校正后本板显示 `UNDERVOLTAGE`；如仍出现，报告它。修改欠压门限或清故障属于另一个操作，不能由校正分压自动推导。

## 输入失灵与恢复

先尝试普通 `status`。正常打开串口会在 `open()` 前设置 `dtr=False`、`rts=False`，不会主动复位。
若只有回显、仅收到末尾字符、没有命令回复或超时，命令是否执行可能未知；不要盲目重发写入或动作命令。
先查询实际状态/参数。如果已无法输入，且当前任务允许重启板子，可显式复位并查询：

```powershell
uv run --project tools python tools/bf_cli.py --serial COM4 --reset --json "status"
```

`--reset` 必须指定 `--serial`。它对 USB-Serial-JTAG 发 RTS 复位序列、关闭旧句柄、等待 Windows 重新枚举后重连，并等待启动探测结束。
复位会中断飞控运行；参数、舵机标定和电流零点保存在 NVS。用户只要求读状态时，不要自动扩大为复位操作。
2026-10-23 曾出现所有输入仅剩末尾字节，显式复位后恢复，连续 12 次关闭/重连查询成功；具体触发条件尚未确定。
复位后的正常 pyserial 收发已验证；如再次复现且复位无效，可对比 PuTTY/Tera Term 或 `idf.py monitor --no-reset`，记录原始收发与端口状态。

禁止为修控制台而 `erase-flash`、擦除 NVS、`param reset` 或覆盖标定。避免以重烧固件代替已验证的前端恢复。

## 维护前端时的约束

- 设备每秒有两条 `I (tick) telemetry: ...` 日志，与回复交错。剔除日志、ANSI、提示符和完整命令回显。
- 保留跨次读取的残缺日志行，避免在每条命令前清空输入缓冲把日志切成无法识别的残片。
- 不依赖提示符判断完成；收到真实回复后，按忽略日志的 `0.45 s` 静默判断结束。无回复必须超时报错。
- 保留前导空格兼容曾观察到的首字节丢失；只发送 CR，设备 VFS 会把 CR 转成换行。不要发送 Ctrl-C、终端探测或多余 CRLF 来同步。
- 不自动重发未知结果的命令；超时后停止该连接上的后续命令。stdout 在 `--json` 命令模式只输出 JSON。
- 命令上限是 158 个 UTF-8 字节，为固件 160 字节命令行中的前导空格和终止符预留空间；拒绝控制字符和嵌入换行。

验证命令：

```powershell
uv run --project tools python -m unittest test.test_bf_cli
```

如沙箱阻止 uv 写全局缓存，可为本次进程设置 `UV_CACHE_DIR` 到仓库内目录；已有 `tools/.venv` 时也可直接运行 `tools/.venv/Scripts/python.exe`。不要切换到其他工具链的全局 Python。
