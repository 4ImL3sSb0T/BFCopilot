"""BetaFPV 小白控三代（LiteRadio 3）USB 摇杆读取。

pygame-ce 用 SDL 直接枚举 HID 设备，不依赖 Windows 旧版 joyGetPosEx。
设备未连接时保持 ``Sticks(connected=False)``，界面仍可用滑块操作。
"""

from __future__ import annotations

import os
import threading
import time
from dataclasses import dataclass

from .mapping import Sticks

os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")


@dataclass
class AxisRoles:
    throttle: int = 2
    yaw: int = 0
    pitch: int = 1
    roll: int = 3
    invert_throttle: bool = False
    invert_yaw: bool = False
    invert_pitch: bool = False
    invert_roll: bool = False
    # 小白控三代的油门机械下限约为 -0.83，不是 -1.0；这两个值由界面校准。
    throttle_low: float = -0.85
    throttle_high: float = 1.0
    arm_button: int | None = None
    estop_button: int | None = None


@dataclass
class DeviceInfo:
    connected: bool = False
    name: str = ""
    guid: str = ""
    axes: int = 0
    buttons: int = 0
    hats: int = 0
    error: str = ""
    raw_axes: tuple[float, ...] = ()
    raw_buttons: tuple[bool, ...] = ()


class JoystickReader:
    def __init__(self, roles: AxisRoles | None = None, rate_hz: float = 100.0) -> None:
        self.roles = roles or AxisRoles()
        self.rate_hz = max(20.0, min(200.0, rate_hz))
        self._lock = threading.Lock()
        self._sticks = Sticks()
        self._info = DeviceInfo()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._joy = None
        self._pygame = None
        self._error = ""

    def configure(self, roles: AxisRoles) -> None:
        with self._lock:
            self.roles = roles

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, name="rc-joystick", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        thread, self._thread = self._thread, None
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1.0)

    def snapshot(self) -> Sticks:
        with self._lock:
            return self._sticks

    def info(self) -> DeviceInfo:
        with self._lock:
            return self._info

    # -- worker ----------------------------------------------------------

    def _run(self) -> None:
        try:
            import pygame
        except Exception as error:  # pragma: no cover - 依赖缺失时允许降级
            self._error = f"pygame 不可用：{error}"
            with self._lock:
                self._info = DeviceInfo(error=self._error)
            return
        self._pygame = pygame
        try:
            # SDL 需要视频子系统才能泵事件；用 1x1 隐藏窗口，不弹界面。
            pygame.display.init()
            pygame.display.set_mode((1, 1), getattr(pygame, "HIDDEN", 0))
            pygame.joystick.init()
        except Exception as error:
            self._error = f"摇杆子系统初始化失败：{error}"
            with self._lock:
                self._info = DeviceInfo(error=self._error)
            return

        period = 1.0 / self.rate_hz
        next_probe = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if self._joy is None and now >= next_probe:
                self._open(pygame)
                next_probe = now + 1.0
            if self._joy is not None:
                self._poll(pygame)
            self._stop.wait(period)
        self._close(pygame)

    def _open(self, pygame) -> None:
        try:
            count = pygame.joystick.get_count()
        except Exception as error:
            self._mark_error(f"枚举摇杆失败：{error}")
            return
        if count <= 0:
            with self._lock:
                self._info = DeviceInfo(error="未找到摇杆，插入 BetaFPV 小白控三代后会自动重试")
                self._sticks = Sticks(connected=False)
            return
        chosen = None
        names: list[tuple[int, str]] = []
        for index in range(count):
            try:
                candidate = pygame.joystick.Joystick(index)
                name = candidate.get_name()
            except Exception:
                continue
            names.append((index, name))
            if chosen is None and any(key in name.lower() for key in ("betafpv", "literadio", "beta fpv")):
                chosen = candidate
        if not names:
            self._mark_error("摇杆存在但无法打开，等待重新连接")
            return
        if chosen is None:
            chosen = pygame.joystick.Joystick(names[0][0])
        try:
            chosen.init()
        except Exception as error:
            self._mark_error(f"打开摇杆失败：{error}")
            return
        self._joy = chosen
        with self._lock:
            self._info = DeviceInfo(
                connected=True,
                name=chosen.get_name(),
                guid=getattr(chosen, "get_guid", lambda: "")(),
                axes=chosen.get_numaxes(),
                buttons=chosen.get_numbuttons(),
                hats=chosen.get_numhats(),
            )
            self._sticks = Sticks(connected=True, name=self._info.name)

    def _poll(self, pygame) -> None:
        try:
            pygame.event.pump()
            if pygame.joystick.get_count() <= 0:
                raise RuntimeError("设备数量变为 0")
        except Exception as error:
            self._mark_error(f"摇杆读取失败：{error}")
            self._disconnect()
            return
        try:
            axes = tuple(self._joy.get_axis(index) for index in range(self._joy.get_numaxes()))
            buttons = tuple(bool(self._joy.get_button(index))
                            for index in range(self._joy.get_numbuttons()))
        except Exception as error:
            self._mark_error(f"摇杆断开：{error}")
            self._disconnect()
            return
        roles = self.roles
        throttle_raw = axes[roles.throttle] if 0 <= roles.throttle < len(axes) else -1.0
        low, high = roles.throttle_low, roles.throttle_high
        if high <= low + 0.05:
            low, high = -1.0, 1.0
        throttle = (throttle_raw - low) / (high - low)
        if roles.invert_throttle:
            throttle = 1.0 - throttle
        throttle = max(0.0, min(1.0, throttle))
        yaw = self._axis(axes, roles.yaw, roles.invert_yaw)
        pitch = self._axis(axes, roles.pitch, roles.invert_pitch)
        roll = self._axis(axes, roles.roll, roles.invert_roll)
        with self._lock:
            self._sticks = Sticks(
                throttle=throttle, yaw=yaw, pitch=pitch, roll=roll,
                connected=True, name=self._info.name,
                axes=axes, buttons=buttons, timestamp=time.monotonic(),
            )
            info = self._info
            self._info = DeviceInfo(
                connected=True, name=info.name, guid=info.guid, axes=info.axes,
                buttons=info.buttons, hats=info.hats, raw_axes=axes, raw_buttons=buttons,
            )

    @staticmethod
    def _axis(axes: tuple[float, ...], index: int, invert: bool) -> float:
        if not 0 <= index < len(axes):
            return 0.0
        value = max(-1.0, min(1.0, axes[index]))
        return -value if invert else value

    def _disconnect(self) -> None:
        self._joy = None
        with self._lock:
            self._sticks = Sticks(connected=False)
            self._info = DeviceInfo(error="摇杆已断开，等待重新连接")

    def _mark_error(self, text: str) -> None:
        self._error = text
        with self._lock:
            info = self._info
            self._info = DeviceInfo(connected=False, name=info.name, guid=info.guid,
                                    axes=info.axes, buttons=info.buttons, hats=info.hats,
                                    error=text)

    def _close(self, pygame) -> None:
        joy, self._joy = self._joy, None
        if joy is not None:
            try:
                joy.quit()
            except Exception:
                pass
        try:
            pygame.joystick.quit()
            pygame.display.quit()
        except Exception:
            pass
