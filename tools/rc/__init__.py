"""BFCopilot 专用遥控上位机。

与台架调试站（station）相互独立：这里只做 PC ↔ Wi-Fi/UDP ↔ 飞控的遥控链路，
不连接台架固件，也不复用台架 console。
"""

__all__ = ["protocol", "transport", "session", "mapping", "joystick", "worker"]
