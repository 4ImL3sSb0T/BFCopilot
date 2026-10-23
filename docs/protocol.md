# BFCopilot 协议 v1

本协议用于开环台架软件。线格式以本文件及 `src/service/protocol` 为准；不是飞行验收记录。

帧固定 60 字节、小端，无 C 结构体直接传输。头 24 字节：magic(u16=0x4642)、version(u8=1)、type(u8)、vehicle_id(u32=1)、session_id(u32)、sequence(u32)、request_id(u32)、payload_len(u16=32)、reserved(u16=0)。payload 固定 32 字节；尾 CRC32(u32)，覆盖前 56 字节，CRC-32/ISO-HDLC。

消息：HELLO=1、CONTROL=2、ARM=3、DISARM=4、ESTOP=5、CLEAR=6、STATUS=7、ACK=8。HELLO 的 session=0，request_id 为随机非零连接 nonce。飞控每次 HELLO 先停止旧会话，再签发随机非零 session；STATUS 回显 nonce。未定义的 payload 字节必须为零。除 HELLO 外，请求 session 必须匹配、sequence 严格递增且不得回绕。ESTOP 可使用 session=0，但仍必须来自加密白名单 peer；不刷新控制活性。

CONTROL payload：mode(u8，0=保持中心、1=手动角度、2=正弦扑翼)、reserved(u8)、ttl_ms(u16，1..100)、upstream_age_ms(u16)、reserved(u16)、lease_id(u32)、frequency_millihz(u32)、左右 amplitude_centideg(u16×2)、左右 center_centideg(i16×2)、phase_centideg(i16)、左右 trim_centideg(i16×2)、differential_permille(i16，-1000..1000)。差动：左幅度乘 (1+d)，右幅度乘 (1-d)，限位检查使用调整后的幅度。mode 0/1 要求频率、幅度、相位、差动为零。所有模式检查全行程；扑翼检查峰值速度和加速度。未知模式一律拒绝。

STATUS payload：lease_id(u32)、lease_valid_for_ms(u16)、safety_state(u8)、armed(u8)、faults(u32)、vbat_mv(i32)、iservo_ma(i32)、左右输出指令 centideg(i16×2)、phase_centideg(u16)、rx_age_ms(u16)、rejected_frames(u32)。无效电源读数为 -1；舵机数值仅为输出指令，无实际位置反馈。每 20ms 签发 lease，有效期 150ms，保存八个本地到期时间；发送排队不能延长 lease。有效 CONTROL 的本地截止时间取 TTL 剩余时间与 lease 剩余时间较小值。超时清会话并锁定，恢复后必须 HELLO、CLEAR、全新 CONTROL、显式 ARM。HEARTBEAT 不存在，ACK/STATUS 不续期。

ARM/CLEAR 必须携带非零 request_id；要求新鲜 CONTROL。ARM 要求零幅度保持/手动命令，不允许直接以扑翼命令解锁。按最近 request_id 缓存 ACK；重复请求只重发结果，不执行状态变更。ACK payload 前 4 字节为结果（0=成功、1=拒绝），第 5 字节为请求类型。普通 CONTROL 不重传，不队列积压。ESTOP 单独锁存，接收队列满也不丢急停。

无线：ESP-NOW 加密单播，共同固定信道、peer MAC 白名单、16字节 PMK/LMK，默认禁用，必须通过 menuconfig 显式配置。当前以构建配置进行有线部署配对（密钥进入固件，sdkconfig 不入 Git）；生产部署需安全烧录与 NVS 配对管理，尚未实现无线配对/OTA。不要分享 sdkconfig 或固件中的密钥。

遥控端 USB：同一 60 字节帧，COBS 编码后 0x00 分隔；UART0 115200 8N1，适用于外置 USB-UART 桥，原生 USB 端口不可假设兼容。最大编码长度 61，部分帧超过 100ms 丢弃，溢出后丢至分隔符。遥控端只转发新收到的帧，绝不周期续发旧控制；PC 停发后飞控 TTL 到期锁定。串口接收和无线发送采用有界缓存。

### Wi-Fi UDP 直连（遥控上位机）

`tools/bf_rc.py` 使用一条与 USB/ESP-NOW 并列的传输：飞控以 AP 模式提供专用热点，电脑连接该热点后通过 UDP 直接收发同一 60 字节帧。

- 每个 UDP 数据报恰好承载一个 60 字节帧，**不做 COBS**（数据报本身提供边界）；帧头、payload、CRC 与本文其余部分完全一致。
- 飞控监听 `CONFIG_BF_WIFI_LINK_PORT`（默认 3335），并把 STATUS/ACK 回复到最近一次有效数据报的源地址和端口；电脑使用临时本地端口即可。
- 发现：电脑广播 ASCII `BF-RC-DISCOVER`，飞控回复 `BF-RC-HERE`。发现只用于找 IP，不建立会话，不改变控制状态。
- 会话、序号、lease、TTL、ACK 幂等和失联规则与 ESP-NOW 完全相同；Wi-Fi 只替换传输层，不绕过 `flight` 的任何校验。控制帧和 STATUS 均为 50 Hz，控制 TTL 100 ms，lease 150 ms，遥测 150 ms 未更新即关闭会话。Wi-Fi 回包由独立发送任务处理，控制任务只做非阻塞入队；队列满或排队达到 20 ms 时丢弃，不延长租约。
- 安全边界：当前只用 WPA2 热点隔离链路，**没有应用层认证**。同一热点上的任何客户端都能按本协议发帧，因此这条链路只用于台架/室内调试，不作为可飞行链路。热点密码放在本地构建配置，不进入 Git；生产部署需要额外的配对与消息认证。

飞控侧构建选项：`CONFIG_BF_WIFI_LINK_ENABLED=y`，并配置 `BF_WIFI_AP_SSID`、`BF_WIFI_AP_PASSWORD`、`BF_WIFI_LINK_PORT`、`BF_WIFI_AP_CHANNEL`。该选项与 `BF_RADIO_ENABLED` 互斥，默认关闭。

有人看护的台架 Wi-Fi 可显式启用 `CONFIG_BF_WIFI_LINK_BENCH_TIMING=y`（默认关闭）：CONTROL 的 TTL 上限改为 300 ms，lease 有效期改为 500 ms，按 20 ms 周期保存 25 个 lease。其余校验、到期回锁和 ESTOP 优先处理保持生效。此配置会延后失联保护，不适用于飞行。上位机仅在匹配 HELLO nonce 的首次 STATUS 报告 500 ms lease 时选择此配置，并显示台架提示；本地遥测超时相应为 500 ms。重新连接先恢复严格配置，不能由普通 STATUS 动态放宽。未启用该选项时仍为 TTL 100 ms、lease/遥测超时 150 ms。

台架固件（`test/bench`）**不实现本协议**：它是硬件测试工具，跑的是控制台而不是控制帧——没有会话、没有 lease、没有 arm，也不链接 `flight`/`safety`/`radio`/`protocol`。产品链路（USB 帧协议、遥控端、ESP-NOW）不受影响，本节其余内容照旧。台架固件的定位、`hw` 命令面与上位机见 `docs/PLAN.md` 实施记录。
