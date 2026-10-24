# BSP 网络测试

`src/app/net_test.c` 是板端测试任务，默认关闭。打开 `BFCopilot app → Enable BSP network test task` 后，板端会在 Wi-Fi 连接成功时启动：

- TCP 服务端：`CONFIG_BF_NET_TEST_TCP_PORT`，默认 `3333`
- UDP 服务端：`CONFIG_BF_NET_TEST_UDP_PORT`，默认 `3334`
- 可选 TCP 客户端：打开 `Enable network test TCP client`，把目标主机和端口指向运行测试脚本的电脑

电脑端运行：

```powershell
& .\tools\.venv\Scripts\python.exe tools\bf_net_test.py --host 192.168.137.100
```

默认脚本会验证 TCP 分段回显、1024 字节边界、TCP 重连、UDP 空报文和最大报文回显。若同时打开板端 TCP 客户端，在电脑端增加监听端口：

```powershell
& .\tools\.venv\Scripts\python.exe tools\bf_net_test.py --host 192.168.137.100 --client-listen-port 3335
```

此时板端 `Network test TCP client host` 必须填写电脑在同一局域网中的 IPv4 地址，端口填写 `3335`。成功时脚本输出 `PASS tcp=... udp=... client=...`，板端日志会周期打印各方向通过计数。

压力测试会持续并发 TCP 回显，同时以突发方式发送 UDP 报文，UDP 丢包率会被统计并输出；TCP 数据错误、UDP 异常回包会直接失败：

```powershell
& .\tools\.venv\Scripts\python.exe tools\bf_net_test.py --host 192.168.137.100 --mode stress --duration 60 --tcp-workers 8 --udp-workers 4 --udp-burst 32
```

可靠性测试会反复建立/关闭 TCP 连接，并逐包确认 UDP 回显，适合验证重连、资源释放和长时间稳定性：

```powershell
& .\tools\.venv\Scripts\python.exe tools\bf_net_test.py --host 192.168.137.100 --mode reliability --reconnects 500 --reliability-messages 2000
```

也可以用 `--mode all` 依次执行基础、可靠性和压力测试。板端日志中的 `send_failures` 与 `client_mismatch` 应保持为 `0`；压力测试期间 UDP 丢包率取决于无线链路和发送强度，需要结合目标场景记录。
