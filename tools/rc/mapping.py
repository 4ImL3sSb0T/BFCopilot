"""摇杆 → 飞控命令的纯映射逻辑。

这里不碰 Tkinter、socket 或 pygame，方便主机测试。
"""

from __future__ import annotations

import math
from dataclasses import dataclass, replace

from .protocol import FlapCommand, FlapLimits, sanitize, validate

MODE_HOLD = 0
MODE_MANUAL = 1
MODE_FLAP = 2
MODE_NAMES = {MODE_HOLD: "保持", MODE_MANUAL: "手动角度", MODE_FLAP: "扑翼"}
MODE_LABELS = {MODE_HOLD: "保持中位", MODE_MANUAL: "手动角度", MODE_FLAP: "正弦扑翼"}
MODE_CODES = {"hold": MODE_HOLD, "manual": MODE_MANUAL, "flap": MODE_FLAP}


@dataclass(frozen=True)
class Sticks:
    """一帧摇杆输入。throttle 为 0..1，其余为 -1..1。"""

    throttle: float = 0.0
    yaw: float = 0.0
    pitch: float = 0.0
    roll: float = 0.0
    connected: bool = False
    name: str = ""
    axes: tuple[float, ...] = ()
    buttons: tuple[bool, ...] = ()
    timestamp: float = 0.0

    @property
    def throttle_low(self) -> bool:
        return self.throttle <= 0.05


@dataclass
class MappingConfig:
    """摇杆输入到扑翼参数的映射与手感设置。"""

    deadband: float = 0.04
    expo: float = 0.25
    center_range_deg: float = 30.0
    max_frequency_hz: float = 8.0
    phase_range_deg: float = 90.0
    differential_range: float = 0.6
    amplitude_deg: float = 20.0
    amplitude_left_deg: float | None = None
    amplitude_right_deg: float | None = None
    use_joystick: bool = True
    require_joystick: bool = True

    def amplitude_for(self, side: int) -> float:
        separate = self.amplitude_left_deg if side == 0 else self.amplitude_right_deg
        return self.amplitude_deg if separate is None else separate


@dataclass
class ManualControls:
    """不开摇杆时，界面滑块给出的命令。"""

    mode: int = MODE_HOLD
    frequency_hz: float = 0.0
    amplitude_left_deg: float = 20.0
    amplitude_right_deg: float = 20.0
    center_left_deg: float = 0.0
    center_right_deg: float = 0.0
    phase_deg: float = 0.0
    differential: float = 0.0

    def command(self) -> FlapCommand:
        return FlapCommand(
            mode=self.mode,
            frequency_hz=self.frequency_hz,
            amplitude_deg=(self.amplitude_left_deg, self.amplitude_right_deg),
            center_deg=(self.center_left_deg, self.center_right_deg),
            phase_deg=self.phase_deg,
            differential=self.differential,
        )


def shape(value: float, deadband: float, expo: float) -> float:
    """死区 + 指数曲线，输入输出都在 -1..1。"""
    if not math.isfinite(value):
        return 0.0
    value = max(-1.0, min(1.0, value))
    deadband = max(0.0, min(0.9, deadband))
    expo = max(0.0, min(1.0, expo))
    magnitude = abs(value)
    if magnitude <= deadband:
        return 0.0
    scaled = (magnitude - deadband) / (1.0 - deadband)
    return math.copysign(scaled ** (1.0 + expo), value)


def sticks_to_command(sticks: Sticks, config: MappingConfig, mode: int) -> FlapCommand:
    """把当前摇杆位置映射成命令。

    保持/手动：俯仰 → 公共中心角，横滚 → 左右中心角差；频率/幅度/相位/差动为 0。
    扑翼：油门 → 频率，俯仰 → 公共中心角，横滚 → 差动，偏航 → 相位，
    幅度来自设置里的基础幅度（左右可分开）。
    """
    yaw = shape(sticks.yaw, config.deadband, config.expo)
    pitch = shape(sticks.pitch, config.deadband, config.expo)
    roll = shape(sticks.roll, config.deadband, config.expo)
    throttle = max(0.0, min(1.0, sticks.throttle))
    center = pitch * config.center_range_deg

    if mode != MODE_FLAP:
        left = center - roll * config.center_range_deg * 0.5
        right = center + roll * config.center_range_deg * 0.5
        return FlapCommand(mode=mode, center_deg=(left, right))

    frequency = throttle * config.max_frequency_hz
    amplitude = (config.amplitude_for(0), config.amplitude_for(1))
    return FlapCommand(
        mode=MODE_FLAP,
        frequency_hz=frequency,
        amplitude_deg=amplitude,
        center_deg=(center, center),
        phase_deg=yaw * config.phase_range_deg,
        differential=roll * config.differential_range,
    )


def build_command(sticks: Sticks, config: MappingConfig,
                  manual: ManualControls, use_joystick: bool | None = None) -> FlapCommand:
    """按当前输入源生成命令，并做一次本地收敛。"""
    joystick = config.use_joystick if use_joystick is None else use_joystick
    if joystick and sticks.connected:
        return sticks_to_command(sticks, config, manual.mode)
    return manual.command()


def prepare(command: FlapCommand, limits: FlapLimits) -> tuple[FlapCommand, list[str]]:
    """收敛到本地限幅，返回可直接编码的命令和提示。"""
    return sanitize(command, limits)


def command_ok(command: FlapCommand, limits: FlapLimits) -> tuple[bool, str]:
    return validate(command, limits)


def arm_blockers(*, connected: bool, session: bool, state: int, armed: bool,
                 faults: int, throttle: float, joystick_connected: bool,
                 require_joystick: bool, mode: int) -> list[str]:
    """列出阻止解锁的原因；空列表表示可以请求解锁。"""
    reasons: list[str] = []
    if not connected or not session:
        reasons.append("未建立控制会话")
    if armed:
        return reasons
    if state != 1:
        from .protocol import SAFETY_LABELS
        reasons.append(f"飞控状态为 {SAFETY_LABELS.get(state, state)}，不是待命锁定")
    if faults:
        reasons.append("飞控存在故障，需要先清除")
    if mode == MODE_FLAP:
        reasons.append("扑翼模式不能直接解锁，先切到保持/手动")
    if throttle > 0.05:
        reasons.append("油门未归零")
    if require_joystick and not joystick_connected:
        reasons.append("摇杆未连接")
    return reasons


def mode_allowed(armed: bool, mode: int) -> tuple[bool, str]:
    if mode not in (MODE_HOLD, MODE_MANUAL, MODE_FLAP):
        return False, "未知模式"
    if mode == MODE_FLAP and not armed:
        return False, "未解锁时只允许保持/手动模式"
    return True, "ok"


def clamp_manual(manual: ManualControls, limits: FlapLimits) -> ManualControls:
    low_freq = 0.0
    high_freq = limits.max_frequency_hz
    return replace(
        manual,
        frequency_hz=max(low_freq, min(high_freq, manual.frequency_hz)),
        amplitude_left_deg=max(0.0, min(limits.max_deg[0] - limits.min_deg[0],
                                          manual.amplitude_left_deg)),
        amplitude_right_deg=max(0.0, min(limits.max_deg[1] - limits.min_deg[1],
                                           manual.amplitude_right_deg)),
        center_left_deg=max(limits.min_deg[0], min(limits.max_deg[0], manual.center_left_deg)),
        center_right_deg=max(limits.min_deg[1], min(limits.max_deg[1], manual.center_right_deg)),
        phase_deg=max(-180.0, min(180.0, manual.phase_deg)),
        differential=max(-1.0, min(1.0, manual.differential)),
    )
