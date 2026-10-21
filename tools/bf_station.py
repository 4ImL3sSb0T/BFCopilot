#!/usr/bin/env python3
"""BFCopilot 原生桌面硬件调试站。

    uv run --project tools python tools/bf_station.py --port COM4
"""

import argparse

from station.link import Link, available_ports


def main():
    parser = argparse.ArgumentParser(description="BFCopilot Python 桌面硬件调试站")
    parser.add_argument("--port", help="控制台串口（USB-Serial-JTAG）；留空时自动搜索")
    parser.add_argument("--list", action="store_true", help="列出可用串口后退出")
    args = parser.parse_args()
    if args.list:
        for device in available_ports():
            print(device)
        return
    from station.gui import Station

    Station(Link(args.port)).mainloop()


if __name__ == "__main__":
    main()
