#!/usr/bin/env python3
"""BFCopilot 原生桌面硬件调试站。

    uv run --project tools python tools/bf_station.py --host 192.168.137.100
"""

import argparse

from station.link import Link, discover


def main():
    parser = argparse.ArgumentParser(description="BFCopilot Python 桌面硬件调试站")
    parser.add_argument("--host", help="开发板 IP；留空时自动搜索")
    parser.add_argument("--port", type=int, default=3333, help="TCP 控制台端口")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("端口必须在 1–65535 之间")
    from station.gui import Station

    Station(Link(args.host, args.port)).mainloop()


if __name__ == "__main__":
    main()
