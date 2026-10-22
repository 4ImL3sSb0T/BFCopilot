#pragma once

/*
 * com 层统一入口：Wi-Fi、TCP/UDP、串口。
 *
 * 使用方只 #include "bsp_com.h" 即可，不必关心三个模块的头文件名字。
 */

#include "bsp_net.h"
#include "bsp_uart.h"
#include "bsp_wifi.h"
