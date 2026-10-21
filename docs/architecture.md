# 四层架构与桌面上位机

固件代码集中在 `src/`，按 `app → service → driver → bsp` 组织。上层可以直接使用更低层接口；
下层不能包含上层头文件。每层只有一个构建注册文件，业务模块不再各自注册组件。
ESP-IDF 的组件注册仅作为底层构建机制。

| 层 | 目录内容 | 职责 |
|---|---|---|
| app | app_main、remote_main、flight、bf_console | 入口、任务编排、会话控制与操作命令 |
| service | parameter、safety、flapping、protocol、radio、telemetry | 参数持久化、安全规则、轨迹、协议与消息队列 |
| driver | servo、sensors、espnow、lsm6ds3tr_c | PWM、ADC、SPI/I2C 器件与加密无线传输 |
| bsp | board、common | 引脚、板级能力、指示灯/蜂鸣器、CRC、故障词汇与运行锁 |

## 配置与并发

舵机、传感器驱动不调用参数服务。应用初始化时将
`parameter_get()->servo`、`&parameter_get()->sensors` 注入驱动。
配置类型定义在驱动公开头文件中，参数服务复用这些类型；NVS 数据布局和 schema 保持原样。
配置指针必须在驱动整个生命周期内有效。当前参数存储的地址稳定，
参数提交更新同一对象，因此调参后驱动可读取新值。调参、驱动读写仍由
`bf_runtime_lock()` 串行保护；重新加载接口会关闭 PWM 或清空采样滤波器。

`src/driver/espnow` 只处理来源白名单、加密和字节传输，通过回调交给
`src/service/radio`。后者做帧校验、时间戳入队、溢出统计及独立急停锁存。
帧线格式、会话有效期和禁止自动解锁的规则保持不变。

## 三种固件

完整 Windows 构建、烧录、串口监视及本地配置说明见 [flashing.md](flashing.md)。
硬件调试直接运行 `scripts/flash-bench.cmd -Port COM4`。

根工程从 `src/app/app_main.c` 构建飞控；`-D BF_REMOTE=1` 切换为
`src/app/remote_main.c`。台架工程使用 `test/bench/src/app`，共享 BSP、
器件驱动、参数服务与轨迹生成器，仅编译自己的网络控制台和硬件测试命令。
`BF_BENCH` 在配置时排除无线传输、飞行协议、安全、遥测及飞控应用。
台架仍不运行飞行逻辑：轨迹生成器只在 `flap` 命令期间工作，且不看任何解锁或故障状态；
台架裸脉宽接口仍可绕过标定。

迁移后首次构建使用新构建目录，避免旧路径缓存：

```powershell
scripts/idf.cmd -B build-flight build
scripts/idf.cmd -B build-remote -D BF_REMOTE=1 build
scripts/idf.cmd -C test/bench -B C:/Users/yuang/Projects/BFCopilot/test/bench/build build
cmake -S test/host -B test/host/build
cmake --build test/host/build
ctest --test-dir test/host/build --output-on-failure
uv run --project tools python -m unittest discover -s test -p "test_*.py"
```

无线配置仍使用本地 defaults；不要将实际凭据提交。台架固件已不使用 Wi-Fi，
其控制台走板载 USB-Serial-JTAG 串口，因此没有台架凭据文件。
如运行环境不允许访问系统 ccache 目录，可以将 `CCACHE_DIR` 指向项目的
`.cache/ccache`。现有 `docs/diagrams` 是旧版组件架构的历史图，不代表当前目录结构。
目录迁移完成后运行 `codegraph sync` 更新索引。

## Python 桌面调试站

`tools/bf_station.py` 是 Tkinter 原生窗口，直接连接开发板的 USB-Serial-JTAG 控制台串口。
传输在 `tools/station/link.py`（pyserial），窗口及工作线程在 `tools/station/gui.py`。
已移除 HTTP 服务、浏览器启动和 `station.html`，也不再使用 TCP。

```powershell
uv run --project tools python tools/bf_station.py
uv run --project tools python tools/bf_station.py --port COM4
uv run --project tools python tools/bf_station.py --list
```

Python 需要可用的 Tcl/Tk。当前机器的 uv 托管 Python 无法初始化 Tcl，
已用现有 Espressif Python 验证窗口，可直接使用：

```powershell
uv run --no-project --python C:/Espressif/tools/python/python.exe python tools/bf_station.py
```

窗口包含自动寻板/端口选择、实时原始读取、IMU/气压计/ADC、左右独立及同时裸 PWM、
LED/蜂鸣器/GPIO、SPI/I2C 寄存器读写、原始控制台、日志导出。
操作按钮全部使用 `hw` 底层命令，不要求标定或解锁。舵机输入单位为 µs，
支持 ±10/±50 µs 点动，按发送才输出；不将未标定脉宽冒充实际角度。
ADC 显示计数和引脚毫伏值，电池电压/电流换算另属参数标定功能。
气压计即使 ID 不匹配，台架也会继续读取可响应器件的原始寄存器。
台架 `status` 同样使用原始读数；需要调参时可在原始控制台使用 `param` 命令。
自动搜索逐个探测串口（跳过蓝牙串口，Espressif USB-Serial-JTAG 端口优先），要求收到
完整提示符才认为连上；手动指定的端口不会被替换。
所有串口操作由单个后台线程串行执行，执行期间不积压操作命令；
实时轮询也不会堆积。命令读取完整提示符才算完成，断线、超时或超长响应
会关闭连接并显示错误；不会自动重发写命令。

关闭窗口会终止连接。台架舵机输出保持行为来自固件，
关闭窗口不会自动关闭 PWM，需要操作“停止全部 PWM”或重启开发板。
`tools/bf_ground.py` 保留为串口飞控协议命令行工具。
