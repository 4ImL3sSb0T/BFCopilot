"""BFCopilot 遥控协议。

帧格式与 ``docs/protocol.md`` 一致：60 字节、小端、CRC-32/ISO-HDLC。
Wi-Fi UDP 传输每个数据报直接承载一个 60 字节帧，不做 COBS。
"""

from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass, replace

HELLO, CONTROL, ARM, DISARM, ESTOP, CLEAR, STATUS, ACK = range(1, 9)
KIND_NAMES = {
    HELLO: "HELLO",
    CONTROL: "CONTROL",
    ARM: "ARM",
    DISARM: "DISARM",
    ESTOP: "ESTOP",
    CLEAR: "CLEAR",
    STATUS: "STATUS",
    ACK: "ACK",
}

FRAME_SIZE = 60
PAYLOAD_SIZE = 32
MAX_TTL_MS = 100
LEASE_MS = 150
VEHICLE_ID = 1
WIRE_FLOAT_MARGIN = 1e-6
DISCOVER = b"BF-RC-DISCOVER"
DISCOVER_REPLY = b"BF-RC-HERE"

HEADER = struct.Struct("<HBBIIIIHH")
CONTROL_PAYLOAD = struct.Struct("<BBHHHIIHHhhhhhh")
STATUS_PAYLOAD = struct.Struct("<IHBBIiihhHHI")

assert HEADER.size == 24
assert CONTROL_PAYLOAD.size == PAYLOAD_SIZE
assert STATUS_PAYLOAD.size == PAYLOAD_SIZE

SAFETY_STATES = (
    "BOOT_LOCKED",
    "READY_LOCKED",
    "ARMED",
    "SAFE_RETURN_LOCKED",
    "LOCKED",
    "FAULT_LATCHED",
)
SAFETY_LABELS = {
    0: "上电锁定",
    1: "待命锁定",
    2: "已解锁",
    3: "安全回位",
    4: "已锁定",
    5: "故障锁定",
}

FAULT_NAMES = (
    "LINK_LOST",
    "ESTOP",
    "SERVO_OVERCURRENT",
    "SERVO_STALL",
    "UNDERVOLTAGE",
    "OVERVOLTAGE",
    "ADC_SATURATED",
    "ADC_UNRELIABLE",
    "IMU_LOST",
    "IMU_UNHEALTHY",
    "BARO_LOST",
    "TASK_WDT",
    "RESET_ABNORMAL",
    "PARAM_CORRUPT",
    "NOT_CALIBRATED",
    "INTERNAL",
)
FAULT_LABELS = {
    "LINK_LOST": "控制链路丢失",
    "ESTOP": "急停",
    "SERVO_OVERCURRENT": "舵机过流",
    "SERVO_STALL": "舵机堵转",
    "UNDERVOLTAGE": "欠压",
    "OVERVOLTAGE": "过压",
    "ADC_SATURATED": "ADC 饱和",
    "ADC_UNRELIABLE": "电源测量不可信",
    "IMU_LOST": "IMU 离线",
    "IMU_UNHEALTHY": "IMU 数据异常",
    "BARO_LOST": "气压计离线",
    "TASK_WDT": "任务超时",
    "RESET_ABNORMAL": "上次异常复位",
    "PARAM_CORRUPT": "参数损坏",
    "NOT_CALIBRATED": "未标定",
    "INTERNAL": "内部错误",
}


def cobs_encode(data: bytes) -> bytes:
    output = bytearray([0])
    marker = 0
    code = 1
    for value in data:
        if value == 0:
            output[marker] = code
            marker = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 255:
                output[marker] = code
                marker = len(output)
                output.append(0)
                code = 1
    output[marker] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    output = bytearray()
    position = 0
    while position < len(data):
        code = data[position]
        position += 1
        if not code or position + code - 1 > len(data):
            raise ValueError("invalid COBS")
        chunk = data[position:position + code - 1]
        if 0 in chunk:
            raise ValueError("embedded delimiter")
        output.extend(chunk)
        position += code - 1
        if code != 255 and position < len(data):
            output.append(0)
    return bytes(output)


@dataclass(frozen=True)
class Frame:
    kind: int
    session: int = 0
    sequence: int = 0
    request: int = 0
    payload: bytes = bytes(PAYLOAD_SIZE)

    def encode(self) -> bytes:
        if len(self.payload) != PAYLOAD_SIZE or self.kind not in range(1, 9):
            raise ValueError("invalid frame")
        raw = HEADER.pack(0x4642, 1, self.kind, VEHICLE_ID, self.session,
                          self.sequence, self.request, PAYLOAD_SIZE, 0) + self.payload
        return raw + struct.pack("<I", zlib.crc32(raw) & 0xFFFFFFFF)

    @classmethod
    def decode(cls, raw: bytes) -> "Frame":
        if len(raw) != FRAME_SIZE:
            raise ValueError("length")
        if zlib.crc32(raw[:56]) & 0xFFFFFFFF != struct.unpack_from("<I", raw, 56)[0]:
            raise ValueError("CRC")
        magic, version, kind, vehicle, session, sequence, request, length, reserved = HEADER.unpack_from(raw)
        if (magic, version, vehicle, length, reserved) != (0x4642, 1, VEHICLE_ID, PAYLOAD_SIZE, 0):
            raise ValueError("header")
        if kind not in range(1, 9):
            raise ValueError("type")
        return cls(kind, session, sequence, request, raw[24:56])


def decode_or_none(raw: bytes) -> Frame | None:
    try:
        return Frame.decode(raw)
    except ValueError:
        return None


@dataclass(frozen=True)
class FlapCommand:
    """飞控 flapping 命令的完整语义。

    ``center_deg`` 是最终中心角；协议里的左/右 trim 字段由 payload 拆分，
    上位机默认把 trim 保持为 0，只使用 center。
    """

    mode: int = 0
    frequency_hz: float = 0.0
    amplitude_deg: tuple[float, float] = (0.0, 0.0)
    center_deg: tuple[float, float] = (0.0, 0.0)
    trim_deg: tuple[float, float] = (0.0, 0.0)
    phase_deg: float = 0.0
    differential: float = 0.0

    def with_centers(self, left: float, right: float) -> "FlapCommand":
        return replace(self, center_deg=(left, right))

    def effective_amplitude(self, side: int) -> float:
        sign = 1.0 if side == 0 else -1.0
        return self.amplitude_deg[side] * (1.0 + sign * self.differential)

    def effective_center(self, side: int) -> float:
        return self.center_deg[side] + self.trim_deg[side]


@dataclass(frozen=True)
class FlapLimits:
    """上位机本地限幅。

    这些值只是本地估计，用来避免发出飞控必然拒绝的命令；飞控参数仍是最终依据。
    """

    max_frequency_hz: float = 1.0
    ramp_s: float = 1.0
    min_deg: tuple[float, float] = (-90.0, -90.0)
    max_deg: tuple[float, float] = (90.0, 90.0)
    speed_dps: tuple[float, float] = (180.0, 180.0)
    acceleration_dps2: tuple[float, float] = (360.0, 360.0)

    def max_peak_deg(self, side: int, center: float, frequency_hz: float) -> float:
        travel = min(center - self.min_deg[side], self.max_deg[side] - center)
        if travel < 0:
            return -1.0
        omega = 2.0 * math.pi * max(frequency_hz, 0.0)
        if omega <= 0:
            return travel
        return min(travel, self.speed_dps[side] / omega,
                   self.acceleration_dps2[side] / (omega * omega))


def _finite(*values: float) -> bool:
    return all(math.isfinite(value) for value in values)


def validate(command: FlapCommand, limits: FlapLimits) -> tuple[bool, str]:
    """镜像 ``src/service/flapping/flapping.c:flapping_validate``。"""

    if command.mode not in (0, 1, 2):
        return False, "未知模式"
    if not _finite(command.frequency_hz, command.phase_deg, command.differential,
                   limits.max_frequency_hz, limits.ramp_s, *limits.min_deg,
                   *limits.max_deg, *limits.speed_dps, *limits.acceleration_dps2):
        return False, "存在非有限数值"
    if limits.max_frequency_hz <= 0:
        return False, "最大频率必须大于 0"
    if command.frequency_hz < 0 or command.frequency_hz > limits.max_frequency_hz:
        return False, f"频率超出 0..{limits.max_frequency_hz:g} Hz"
    if abs(command.phase_deg) > 180:
        return False, "相位超出 ±180°"
    if abs(command.differential) > 1:
        return False, "差动超出 ±1"
    if limits.ramp_s <= 0:
        return False, "斜坡时间必须大于 0"
    if command.mode != 2 and (command.frequency_hz != 0 or command.phase_deg != 0
                              or command.differential != 0):
        return False, "保持/手动模式要求频率、相位、差动为 0"
    for side in range(2):
        center = command.effective_center(side)
        peak = command.effective_amplitude(side)
        if not _finite(center, command.amplitude_deg[side], peak):
            return False, "中心角或幅度非有限值"
        if command.amplitude_deg[side] < 0:
            return False, "幅度不能为负"
        if limits.min_deg[side] >= limits.max_deg[side]:
            return False, "限位区间无效"
        if limits.speed_dps[side] <= 0 or limits.acceleration_dps2[side] <= 0:
            return False, "速度/加速度限幅必须大于 0"
        if center - peak < limits.min_deg[side] - 1e-6:
            return False, f"{'左' if side == 0 else '右'}侧低于下限"
        if center + peak > limits.max_deg[side] + 1e-6:
            return False, f"{'左' if side == 0 else '右'}侧高于上限"
        omega = 2.0 * math.pi * command.frequency_hz
        if peak * omega > limits.speed_dps[side] + 1e-6:
            return False, f"{'左' if side == 0 else '右'}侧峰值角速度超限"
        if peak * omega * omega > limits.acceleration_dps2[side] + 1e-6:
            return False, f"{'左' if side == 0 else '右'}侧峰值角加速度超限"
        if command.mode != 2 and peak != 0:
            return False, "保持/手动模式要求幅度为 0"
    return True, "ok"


def _clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def sanitize(command: FlapCommand, limits: FlapLimits) -> tuple[FlapCommand, list[str]]:
    """把摇杆输入收敛到本地限幅内，保证发出的命令不会被飞控整帧拒绝。

    返回收敛后的命令和给界面看的提示。真的无法收敛时退回保持中位。
    """

    issues: list[str] = []
    if command.mode not in (0, 1, 2):
        return FlapCommand(), ["未知模式，已退回保持中位"]
    if not _finite(limits.max_frequency_hz, *limits.min_deg, *limits.max_deg,
                   *limits.speed_dps, *limits.acceleration_dps2):
        return FlapCommand(), ["限幅存在非有限值，已退回保持中位"]
    if limits.max_frequency_hz <= 0:
        return FlapCommand(), ["最大频率必须大于 0，已退回保持中位"]
    mode = command.mode
    frequency = command.frequency_hz
    phase = command.phase_deg
    differential = command.differential
    amplitude = [max(0.0, value) for value in command.amplitude_deg]
    centers = [command.effective_center(0), command.effective_center(1)]

    if mode != 2:
        if frequency or phase or differential or any(amplitude):
            issues.append("保持/手动模式已把频率、幅度、相位、差动清零")
        frequency = phase = differential = 0.0
        amplitude = [0.0, 0.0]
    else:
        if not math.isfinite(frequency):
            frequency = 0.0
        frequency = _clamp(frequency, 0.0, limits.max_frequency_hz)
        frequency = min(round(frequency * 1000.0),
                        math.floor(limits.max_frequency_hz * 1000.0)) / 1000.0
        if not math.isfinite(phase):
            phase = 0.0
        phase = _clamp(phase, -180.0, 180.0)
        phase = round(phase * 100.0) / 100.0
        if not math.isfinite(differential):
            differential = 0.0
        differential = _clamp(differential, -1.0, 1.0)
        differential = round(differential * 1000.0) / 1000.0
        if not math.isfinite(amplitude[0]) or not math.isfinite(amplitude[1]):
            amplitude = [0.0, 0.0]
        if frequency <= 0:
            amplitude = [0.0, 0.0]
            issues.append("扑翼频率为 0，幅度已清零")

    for side in range(2):
        if not math.isfinite(centers[side]):
            centers[side] = 0.0
        low, high = limits.min_deg[side], limits.max_deg[side]
        low_units = max(-32768, math.ceil(low * 100.0))
        high_units = min(32767, math.floor(high * 100.0))
        if low >= high or low_units > high_units:
            return FlapCommand(), ["限位区间无效，已退回保持中位"]
        centers[side] = _clamp(round(centers[side] * 100.0),
                               low_units, high_units) / 100.0

    if mode == 2 and frequency > 0:
        for side in range(2):
            limit = limits.max_peak_deg(side, centers[side], frequency)
            if limit < 0:
                return FlapCommand(), ["中心角超出行程，已退回保持中位"]
            factor = 1.0 + (1.0 if side == 0 else -1.0) * differential
            margin = WIRE_FLOAT_MARGIN * max(1.0, abs(centers[side]),
                                             abs(limits.min_deg[side]),
                                             abs(limits.max_deg[side]), limit)
            allowed_units = 65535 if factor == 0 else math.floor(
                max(0.0, limit - margin) * 100.0 / factor)
            amplitude_units = min(65535, round(amplitude[side] * 100.0), allowed_units)
            quantized_amplitude = amplitude_units / 100.0
            if quantized_amplitude < amplitude[side]:
                issues.append(f"{'左' if side == 0 else '右'}侧幅度收敛到 "
                              f"{quantized_amplitude:.2f}°（当前频率/限幅）")
            amplitude[side] = quantized_amplitude

    result = replace(command, mode=mode, frequency_hz=frequency, phase_deg=phase,
                     differential=differential, amplitude_deg=(amplitude[0], amplitude[1]),
                     center_deg=(centers[0], centers[1]), trim_deg=(0.0, 0.0))
    ok, reason = validate(result, limits)
    if not ok:
        return FlapCommand(), [f"命令无法收敛（{reason}），已退回保持中位"]
    return result, issues


def _centideg(value: float) -> int:
    return int(round(value * 100.0))


def _permille(value: float) -> int:
    return int(round(value * 1000.0))


def _signed16(value: float) -> int:
    return max(-32768, min(32767, _centideg(value)))


def control_payload(lease: int, command: FlapCommand, ttl_ms: int = MAX_TTL_MS,
                    age_ms: int = 0) -> bytes:
    if not 1 <= ttl_ms <= MAX_TTL_MS:
        raise ValueError("ttl_ms 必须在 1..100")
    if not 0 <= age_ms < ttl_ms:
        raise ValueError("age_ms 必须小于 ttl_ms")
    if not 0 <= lease <= 0xFFFFFFFF:
        raise ValueError("lease 越界")
    frequency = command.frequency_hz
    values = (frequency, *command.amplitude_deg, *command.center_deg,
              *command.trim_deg, command.phase_deg, command.differential)
    if not _finite(*values):
        raise ValueError("CONTROL 存在非有限数值")
    return CONTROL_PAYLOAD.pack(
        command.mode, 0, ttl_ms, age_ms, 0, lease,
        int(round(frequency * 1000.0)),
        max(0, min(0xFFFF, _centideg(command.amplitude_deg[0]))),
        max(0, min(0xFFFF, _centideg(command.amplitude_deg[1]))),
        _signed16(command.center_deg[0]), _signed16(command.center_deg[1]),
        _signed16(command.phase_deg),
        _signed16(command.trim_deg[0]), _signed16(command.trim_deg[1]),
        max(-32768, min(32767, _permille(command.differential))),
    )


@dataclass(frozen=True)
class Status:
    lease: int
    lease_ms: int
    state: int
    armed: bool
    faults: int
    vbat_mv: int | None
    iservo_ma: int | None
    left_cd: int
    right_cd: int
    phase_cd: int
    rx_age_ms: int
    rejected: int

    @classmethod
    def from_frame(cls, frame: Frame) -> "Status":
        if frame.kind != STATUS:
            raise ValueError("not STATUS")
        (lease, lease_ms, state, armed, faults, vbat, iservo,
         left, right, phase, rx_age, rejected) = STATUS_PAYLOAD.unpack(frame.payload)
        return cls(lease, lease_ms, state, bool(armed), faults,
                   None if vbat == -1 else vbat,
                   None if iservo == -1 else iservo,
                   left, right, phase, rx_age, rejected)

    @property
    def state_name(self) -> str:
        return SAFETY_STATES[self.state] if 0 <= self.state < len(SAFETY_STATES) else "?"

    @property
    def state_label(self) -> str:
        return SAFETY_LABELS.get(self.state, "?")

    @property
    def fault_names(self) -> list[str]:
        return fault_names(self.faults)


def fault_names(mask: int) -> list[str]:
    return [name for bit, name in enumerate(FAULT_NAMES) if mask & (1 << bit)]


def fault_labels(mask: int) -> list[str]:
    return [FAULT_LABELS.get(name, name) for name in fault_names(mask)]


def frame(kind: int, session: int = 0, sequence: int = 0,
          request: int = 0, payload: bytes = bytes(PAYLOAD_SIZE)) -> Frame:
    return Frame(kind, session, sequence, request, payload)
