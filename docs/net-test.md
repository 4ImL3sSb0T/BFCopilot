# BSP 网络测试

`src/app/net_test.c` 是板端测试任务，默认关闭。打开 `BFCopilot app → Enable BSP network test task` 后，板端会在 Wi-Fi 连接成功时启动：

- TCP 服务端：`CONFIG_BF_NET_TEST_TCP_PORT`，默认 `3333`
- UDP 服务端：`CONFIG_BF_NET_TEST_UDP_PORT`，默认 `3334`
- 可选 TCP 客户端：打开 `Enable network test TCP client`，把目标主机和端口指向运行测试脚本的电脑

电脑端使用 `tools` 项目的 Python 环境运行（开发板 IP 以串口日志为准）：

```powershell
uv run --project tools python tools/bf_net_test.py --host 192.168.137.40
```

默认脚本会验证 TCP 分段回显、1024 字节边界、TCP 重连、UDP 空报文和最大报文回显。若同时打开板端 TCP 客户端，在电脑端增加监听端口：

```powershell
uv run --project tools python tools/bf_net_test.py --host 192.168.137.40 --client-listen-port 3335
```

此时板端 `Network test TCP client host` 必须填写电脑在同一局域网中的 IPv4 地址，端口填写 `3335`。脚本逐阶段输出结果，最后输出 `PASS` 或 `FAIL`，失败退出码为 `1`，参数错误为 `2`。客户端监听覆盖整个测试过程，校验完整载荷及递增序号；`--client-reconnect-every 5` 会每收满五帧主动断开一次，验证板端客户端重新连接。客户端超过 `--client-idle-timeout`（默认十秒）没有新帧即判失败，避免只在开始时成功几帧就误判整个持续测试通过。

## 同时压力与持续运行

压力测试同时运行 TCP 和 UDP；每个 TCP worker 独占一个会话。板端默认总共四个 TCP 槽，启用板端客户端后只剩三个服务端槽。即使电脑不启动客户端监听，也应增加 `--board-client-enabled` 告知脚本这个预留槽。不要使用超过真实槽数的 worker 配置。

下面以三个 TCP 会话、两个各 50 包/秒的 UDP 发送方持续测试五分钟，载荷为 128 字节，UDP 回显丢失阈值为 1%：

```powershell
uv run --project tools python tools/bf_net_test.py --host 192.168.137.40 --mode stress --duration 300 --tcp-workers 3 --udp-workers 2 --udp-burst 1 --udp-rate 50 --stress-payload-size 128 --stress-max-loss 0.01 --client-listen-port 3335 --client-reconnect-every 5 --serial-port COM4 --serial-log .cache/net-soak.serial.log --report-json .cache/net-soak.json
```

`--udp-rate` 是每个 worker 的目标速率，`0` 表示尽力发送。发包和收包分开调度，不再靠等待接收超时来限制发送速率。结束后保留一个 `--timeout` 接收窗口，减少把尾部在途报文误算成丢包。

JSON 保存 TCP 字节数、逐 worker 的运行时长和往返延迟 p50/p95/p99/最大值，以及 UDP 发送、唯一回显、丢失、重复、乱序、损坏、本机发送拥塞、延迟和最大收包间隔。客户端另外保留成功帧数、连接次数与后台错误。每个压力报文携带 worker 和 32 位序号，并校验全部字节；重复回显不会增加成功数量。UDP 丢失表示端到端回显未到达，不能单凭这个数字区分上行、板端和下行丢失。

超过设计负载时可以测量过载行为：

```powershell
uv run --project tools python tools/bf_net_test.py --host 192.168.137.40 --mode stress --duration 60 --tcp-workers 3 --udp-workers 4 --udp-burst 32 --stress-payload-size 1024 --client-listen-port 3335 --serial-port COM4 --serial-log .cache/net-overload.serial.log --report-json .cache/net-overload.json
```

省略 `--stress-max-loss` 时，压力模式只测量 UDP 丢失，不以丢失率判失败；`PASS` 不能解释成零丢包或满足飞控带宽。TCP 错误、UDP 损坏/重复、串口检测到的复位/崩溃仍会失败。配置阈值时，本机发送拥塞也判失败。压力结束后自动检查 TCP/UDP 基础通信能否恢复。

## 重连、原始丢包与异常恢复

可靠性模式重复建立/关闭 TCP，并逐包发送 UDP。UDP 每包只发送一次，超时直接计为丢失，不通过重试掩盖损失；`--max-loss 0` 要求全部回显。序号不会在 256 包后重复，相同长度的坏数据也会失败。

```powershell
uv run --project tools python tools/bf_net_test.py --host 192.168.137.40 --mode reliability --reconnects 1000 --reliability-messages 3000 --max-loss 0 --client-listen-port 3335 --report-json .cache/net-reliability.json
```

`--mode fault` 测试超长 UDP 整包丢弃（1025、2048、8192 字节）、空包与最大合法包恢复、带未读数据的 TCP RST、连接槽耗尽拒绝、慢读取方隔离及恢复；`--fault-rounds` 控制异常断连次数。容量检查以实际板端配置为准。

```powershell
uv run --project tools python tools/bf_net_test.py --host 192.168.137.40 --mode fault --fault-rounds 100 --board-client-enabled --report-json .cache/net-fault.json
```

`--mode all` 按基础、可靠性、异常恢复、混合压力顺序运行；可组合以上参数。报告每完成一个阶段就保存一次，最终错误也写入 JSON。异常恢复期间故意断开连接，板端 TCP 发送失败计数可能增加，应检查后续正常压力阶段是否继续增长，而非要求故障注入期间也保持零。

板端测试任务的服务端和客户端都会处理断连后的未读尾部再释放槽位；BSP 保留尾部是接口约定，业务消费者也必须处理 `connected=false` 的快照。每十秒输出 `health:`，包含当前/历史最低可用堆、最大连续空闲块、测试任务和网络任务的栈余量（ESP-IDF 中单位为字节）、丢弃尾部字节和收发计数。串口监测会记录这些样本，检测复位、panic、看门狗、堆损坏，并检查板端客户端数据错误。

## 主机回归

```powershell
uv run --project tools python -m unittest discover -s test/host -p 'test_*.py' -v
```

主机回归用本地 socket 注入丢包、同长度损坏、重复和错误 worker 回包，同时验证 TCP 整帧截止时间、板端客户端完整载荷校验、参数边界及失败报告落盘。

这些测试是台架通信验证。具体控制周期所需的延迟上限、长时间运行、射频中断恢复、供电扰动及执行机构工作时的干扰需要另设验收条件；当前测试任务保持舵机电源关闭。

本次实测与失败条件见 [2026-10-24 台架记录](net-test-2026-10-24.md)。
