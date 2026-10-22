# BSP + 设备驱动（当前板：蝴蝶控制器 v1）
#
# 引脚与电源拓扑见 docs/hardware-pinmap.md。模块按外设划分：
#   adc   —— 电池电压 / 舵机电流
#   baro  —— SPA06-003 气压计（I2C）
#   buzzer —— 蜂鸣器
#   com   —— 链路：Wi-Fi STA、TCP/UDP 收发、UART 收发
#   imu   —— LSM6DS3TR-C（ST 官方 PID 驱动 + ESP-IDF SPI 粘合层）
#   params —— NVS 键值存储的薄封装
#   servo —— MCPWM 双路舵机与舵机电源开关
idf_component_register(
    SRCS
        "adc/bsp_adc.c"
        "baro/bsp_baro.c"
        "baro/spa06_math.c"
        "buzzer/bsp_buzzer.c"
        "com/bsp_net.c"
        "com/bsp_uart.c"
        "com/bsp_wifi.c"
        "imu/bsp_imu.c"
        "imu/vendor/lsm6ds3tr-c_reg.c"
        "params/bsp_params.c"
        "params/bsp_params_table.c"
        "servo/bsp_servo.c"
    INCLUDE_DIRS
        "adc"
        "baro"
        "buzzer"
        "com"
        "imu"
        "params"
        "servo"
    REQUIRES
        lib
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_mcpwm
        esp_driver_ledc
        esp_adc
    PRIV_REQUIRES
        esp_timer
        freertos
        esp_wifi
        esp_event
        esp_netif
        lwip
        esp_driver_uart
        nvs_flash
)
