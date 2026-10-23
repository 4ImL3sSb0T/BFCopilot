#!/usr/bin/env python3
"""BFCopilot 专用遥控上位机。

电脑通过 Wi-Fi/UDP 直连飞控，BetaFPV 小白控三代经 USB 当摇杆：

    uv run --project tools python tools/bf_rc.py
    uv run --project tools python tools/bf_rc.py --host 192.168.4.1 --port 3335
    uv run --project tools python tools/bf_rc.py --list
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rc.gui import RcStation, load_config  # noqa: E402


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
    parser.add_argument("--host", help="飞控 IP，默认 192.168.4.1")
    parser.add_argument("--port", type=int, default=3335, help="飞控 UDP 端口，默认 3335")
    parser.add_argument("--list", action="store_true", help="列出摇杆后退出")
    parser.add_argument("--no-joystick", action="store_true", help="启动时关闭摇杆输入")
    args = parser.parse_args()
    if args.list:
        return list_joysticks()
    if not 1 <= args.port <= 65535:
        parser.error("端口必须在 1..65535")
    config = load_config()
    config.host = args.host or config.host
    config.port = args.port
    if args.no_joystick:
        config.mapping = dict(config.mapping, use_joystick=False)
    RcStation(config=config).mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
