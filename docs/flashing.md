# 固件构建与烧录

Windows 下从仓库根目录执行。脚本复用 `scripts/idf.cmd` 的 ESP-IDF 6.0.2 工具链，
默认串口 COM4、烧录波特率 460800，构建后写入 bootloader、分区表和应用，成功后复位。
不会擦除整个 Flash 或主动清空 NVS。关闭占用串口的监视器后再烧录。

## 三种固件

```powershell
# 当前硬件测试：含 PWM 修复；Python 上位机优先走 Wi-Fi，连不上时自动改用 USB-Serial-JTAG 串口
scripts/flash-bench.cmd -Port COM4

# 飞控：运行控制与安全状态机
scripts/flash-flight.cmd -Port COM4

# 遥控端：USB/UART 与 ESP-NOW 桥
scripts/flash-remote.cmd -Port COM5
```

直接双击 `flash-bench.cmd` 也会向默认 COM4 烧录，结束后暂停以保留结果和错误信息。
台架和飞控是同一块板的不同固件；遥控端烧在另一块 ESP32-C6 上，不要把三种固件连续烧到同一块板。

## 通用选项

```powershell
scripts/flash-bench.cmd -Port COM4 -Monitor
scripts/flash-bench.cmd -BuildOnly
scripts/flash-bench.cmd -DryRun
scripts/flash-bench.cmd -Port COM4 -Baud 115200
scripts/flash-bench.cmd -Port COM4 -BuildDirectory test/bench/build-refactor
scripts/flash-flight.cmd -DefaultsFile flight.local.defaults -BuildOnly
```

`-Monitor` 烧录成功后进入串口监视器，用 Ctrl+] 退出；`-BuildOnly` 不访问串口；
`-DryRun` 只显示命令，不构建或访问硬件。`-BuildDirectory` 的相对路径从仓库根目录解析，
传给 IDF 的始终是绝对路径。脚本可从其他工作目录执行，失败返回非零退出码。

默认构建目录分别为 `test/bench/build`、`build-flash-flight`、`build-flash-remote`。
台架沿用 `test/bench/sdkconfig`，飞控沿用根目录 `sdkconfig`，遥控端使用其构建目录内的
独立 `sdkconfig`。不要在不同固件间复用同一个构建目录。

## 本地配置

台架自动加载 `test/bench/sdkconfig.defaults` 和存在的 `test/bench/local.defaults`。
控制台固定走板载 USB-Serial-JTAG（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），所以不填
Wi-Fi 凭据也能用串口操作；需要 Wi-Fi 时再复制前者到后者，填写
`CONFIG_BF_WIFI_STA_SSID` 和 `CONFIG_BF_WIFI_STA_PASSWORD`。
现有 sdkconfig 的值优先于 defaults；改 Wi-Fi 或控制台等配置时通过
`scripts/idf.cmd -C test/bench -B <构建目录绝对路径> menuconfig` 更新。

飞控自动加载根目录 `sdkconfig.defaults` 和存在的 `flight.local.defaults`。
遥控端还加载 `scripts/remote.defaults`，启用无线桥并关闭文本串口控制台，
必须提供 `remote.local.defaults` 或 `-DefaultsFile`：

```powershell
Copy-Item scripts/radio.local.defaults.example remote.local.defaults
Copy-Item scripts/radio.local.defaults.example flight.local.defaults
```

填写双方的真实对端 MAC（12 个十六进制字符）以及匹配的 PMK/LMK（各32个十六进制字符）。
示例中的空字符串不能用于通信；不使用 `test/radio.defaults` 中的测试密钥烧录实物。
本地配置已被 Git 忽略。defaults 仅初始化配置；修改已有配置时使用对应构建目录及
SDKCONFIG 的 menuconfig，或保留备份后重新生成配置。

## PWM 修复验证

烧录台架后在上位机执行 `hw servo`，应显示 U1 焊盘及 H1/H2 接口对应关系。
执行 `hw pulse both 1550`，在 H1/H2 第3脚对第1脚测量：约333 Hz、高电平1550 µs。
执行 `hw pulse off` 后应保持低电平，再发送脉宽应恢复输出。
