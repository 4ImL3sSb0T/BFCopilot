# lsm6ds3tr_c

STMicroelectronics 官方 LSM6DS3TR-C 平台无关驱动（PID），原样收录在 `vendor/`。
**不要修改 `vendor/` 下的文件**；升级时重新拉取上游合并。

| 项 | 值 |
|---|---|
| 上游 | https://github.com/STMicroelectronics/lsm6ds3tr-c-pid |
| 版本 | Release version 2.2.3 |
| 提交 | `6735cd1f6ea9208084f2e7dc69605250bc532ac6`（2026-10-08） |
| 许可 | BSD-3-Clause，见 `vendor/LICENSE` |

```
sha256  c4517a99d4d5cc2a589784128f7f84612b1deb66039605869e24d9191057b145  vendor/lsm6ds3tr-c_reg.c
sha256  93a1874a857629138727ec945bb5299e03cfc661b67bab22a26ab5cc300967be  vendor/lsm6ds3tr-c_reg.h
```

驱动只做寄存器读写，总线由使用方提供。上位组件需为 `stmdev_ctx_t` 填三个回调：

```c
int32_t platform_write(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len);
int32_t platform_read(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len);
void    platform_delay(uint32_t millisec);
```

返回值约定：`0` 表示成功，非 0 为错误码。`components/sensors/motion.c` 中的
ESP-IDF SPI 实现即此接口。

## 总线注意点

SPI 读要置地址最高位（`reg | 0x80`），并且 ST 的参考例程用手动 GPIO 拉低 CS
横跨"发地址"和"收数据"两次调用。ESP-IDF 的硬件 CS 每次事务结束都会释放，
所以这里改用 `command_bits = 8`：把地址放进 command phase，数据放进同一个事务的
rx/tx 阶段，一次调用内 CS 保持有效。这与手写驱动此前的事务结构一致。
