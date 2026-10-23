#!/usr/bin/env python3
"""BFCopilot 专用遥控上位机。

电脑通过 Wi-Fi/UDP 连接同一热点下的飞控，BetaFPV 小白控三代经 USB 当摇杆：

    uv run --project tools python tools/bf_rc.py                 # 广播发现飞控
    uv run --project tools python tools/bf_rc.py --host <ip>     # 跳过发现
    uv run --project tools python tools/bf_rc.py --list
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rc.config import load_config  # noqa: E402


def list_joysticks() -> int:
    try:
        import os
        os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
        import pygame
        pygame.display.init()
        pygame.display.set_mode((1, 1), getattr(pygame, "HIDDEN", 0))
        pygame.joystick.init()
    except Exception as error:
        print(f"pygame 不可用：{error}")
        return 1
    count = pygame.joystick.get_count()
    print(f"找到 {count} 个摇杆")
    for index in range(count):
        device = pygame.joystick.Joystick(index)
        print(f"  [{index}] {device.get_name()}  "
              f"axes={device.get_numaxes()} buttons={device.get_numbuttons()} "
              f"hats={device.get_numhats()}")
    return 0 if count else 2


def main() -> int:
    parser = argparse.ArgumentParser(description="BFCopilot Wi-Fi 遥控上位机")
    parser.add_argument("--host", help="飞控 IP；留空则广播发现")
    parser.add_argument("--port", type=int, default=3335, help="飞控 UDP 端口，默认 3335")
    parser.add_argument("--list", action="store_true", help="列出摇杆后退出")
    parser.add_argument("--no-joystick", action="store_true", help="启动时关闭摇杆输入")
    parser.add_argument("--demo", action="store_true", help="预览演示数据，不连接设备")
    parser.add_argument("--legacy", action="store_true", help="打开旧版 Tk 界面")
    parser.add_argument("--web-port", type=int, default=0, help="本机网页端口，默认自动选择")
    parser.add_argument("--no-browser", action="store_true", help="只输出网页地址，不自动打开浏览器")
    args = parser.parse_args()
    if args.list:
        return list_joysticks()
    if not 1 <= args.port <= 65535:
        parser.error("端口必须在 1..65535")
    if not 0 <= args.web_port <= 65535:
        parser.error("网页端口必须在 0..65535")
    config = load_config()
    config.host = args.host or config.host
    config.port = args.port
    if args.no_joystick:
        config.mapping = dict(config.mapping, use_joystick=False)
    if args.legacy:
        if args.demo:
            parser.error("旧版界面不支持演示模式")
        from rc.gui import RcStation
        RcStation(config=config).mainloop()
        return 0
    from rc.web import serve
    return serve(config, demo=args.demo, port=args.web_port, open_browser=not args.no_browser)


if __name__ == "__main__":
    raise SystemExit(main())
