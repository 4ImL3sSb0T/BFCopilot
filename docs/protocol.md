# BFCopilot 协议 v1

本协议用于开环台架软件。线格式以本文件及 `src/service/protocol` 为准；不是飞行验收记录。

帧固定 60 字节、小端，无 C 结构体直接传输。头 24 字节：magic(u16=0x4642)、version(u8=1)、type(u8)、vehicle_id(u32=1)、session_id(u32)、sequence(u32)、request_id(u32)、payload_len(u16=32)、reserved(u16=0)。payload 固定 32 字节；尾 CRC32(u32)，覆盖前 56 字节，CRC-32/ISO-HDLC。

消息：HELLO=1、CONTROL=2、ARM=3、DISARM=4、ESTOP=5、CLEAR=6、STATUS=7、ACK=8。HELLO 的 session=0，request_id 为随机非零连接 nonce。飞控每次 HELLO 先停止旧会话，再签发随机非零 session；STATUS 回显 nonce。未定义的 payload 字节必须为零。除 HELLO 外，请求 session 必须匹配、sequence 严格递增且不得回绕。ESTOP 可使用 session=0，但仍必须来自加密白名单 peer；不刷新控制活性。

CONTROL payload：mode(u8，0=保持中心、1=手动角度、2=正弦扑翼)、reserved(u8)、ttl_ms(u16，1..100)、upstream_age_ms(u16)、reserved(u16)、lease_id(u32)、frequency_millihz(u32)、左右 amplitude_centideg(u16×2)、左右 center_centideg(i16×2)、phase_centideg(i16)、左右 trim_centideg(i16×2)、differential_permille(i16，-1000..1000)。差动：左幅度乘 (1+d)，右幅度乘 (1-d)，限位检查使用调整后的幅度。mode 0/1 要求频率、幅度、相位、差动为零。所有模式检查全行程；扑翼检查峰值速度和加速度。未知模式一律拒绝。

STATUS payload：lease_id(u32)、lease_valid_for_ms(u16)、safety_state(u8)、armed(u8)、faults(u32)、vbat_mv(i32)、iservo_ma(i32)、左右输出指令 centideg(i16×2)、phase_centideg(u16)、rx_age_ms(u16)、rejected_frames(u32)。无效电源读数为 -1；舵机数值仅为输出指令，无实际位置反馈。每 50ms 签发 lease，有效期 150ms，保存四个本地到期时间；发送排队不能延长 lease。有效 CONTROL 的本地截止时间取 TTL 剩余时间与 lease 剩余时间较小值。超时清会话并锁定，恢复后必须 HELLO、CLEAR、全新 CONTROL、显式 ARM。HEARTBEAT 不存在，ACK/STATUS 不续期。

ARM/CLEAR 必须携带非零 request_id；要求新鲜 CONTROL。ARM 要求零幅度保持/手动命令，不允许直接以扑翼命令解锁。按最近 request_id 缓存 ACK；重复请求只重发结果，不执行状态变更。ACK payload 前 4 字节为结果（0=成功、1=拒绝），第 5 字节为请求类型。普通 CONTROL 不重传，不队列积压。ESTOP 单独锁存，接收队列满也不丢急停。

无线：ESP-NOW 加密单播，共同固定信道、peer MAC 白名单、16字节 PMK/LMK，默认禁用，必须通过 menuconfig 显式配置。当前以构建配置进行有线部署配对（密钥进入固件，sdkconfig 不入 Git）；生产部署需安全烧录与 NVS 配对管理，尚未实现无线配对/OTA。不要分享 sdkconfig 或固件中的密钥。

遥控端 USB：同一 60 字节帧，COBS 编码后 0x00 分隔；UART0 115200 8N1，适用于外置 USB-UART 桥，原生 USB 端口不可假设兼容。最大编码长度 61，部分帧超过 100ms 丢弃，溢出后丢至分隔符。遥控端只转发新收到的帧，绝不周期续发旧控制；PC 停发后飞控 TTL 到期锁定。串口接收和无线发送采用有界缓存。

台架固件（`test/bench`）**不实现本协议**：它是硬件测试工具，跑的是控制台而不是控制帧——没有会话、没有 lease、没有 arm，也不链接 `flight`/`safety`/`radio`/`protocol`。产品链路（USB 帧协议、遥控端、ESP-NOW）不受影响，本节其余内容照旧。台架固件的定位、`hw` 命令面与上位机见 `docs/PLAN.md` 实施记录。
