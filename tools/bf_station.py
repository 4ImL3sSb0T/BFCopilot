#!/usr/bin/env python3
"""BFCopilot 原生桌面硬件调试站。

默认先连 Wi-Fi/TCP，连不上再切 USB-Serial-JTAG 串口：

    uv run --project tools python tools/bf_station.py
    uv run --project tools python tools/bf_station.py --host 192.168.137.100
    uv run --project tools python tools/bf_station.py --method serial --serial COM4
"""

import argparse

from station.link import Link, available_ports


def main():
    parser = argparse.ArgumentParser(description="BFCopilot Python 桌面硬件调试站")
    parser.add_argument("--host", help="开发板 IP；留空时自动搜索 Wi-Fi")
    parser.add_argument("--port", type=int, default=3333, help="TCP 控制台端口")
    parser.add_argument("--serial", help="USB-Serial-JTAG 串口（如 COM4）；留空时自动搜索")
    parser.add_argument("--method", choices=("auto", "tcp", "serial"), default="auto",
                        help="连接方式：auto 先 Wi-Fi 再串口（默认），tcp 只用 Wi-Fi，serial 只用串口")
    parser.add_argument("--list", action="store_true", help="列出可用串口后退出")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("端口必须在 1–65535 之间")
    if args.list:
        for device in available_ports():
            print(device)
        return
    from station.gui import Station

    Station(Link(args.host, args.port, args.serial, args.method)).mainloop()


if __name__ == "__main__":
    main()
