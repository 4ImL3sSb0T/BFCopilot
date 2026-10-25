"""会话、lease、序号和 ACK 状态机。

与 ``tools/bf_ground.py`` 的规则一致，但不做 I/O：发送通过回调完成，
时间通过参数传入，方便主机测试。
"""

from __future__ import annotations

import secrets
import struct
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable

from .protocol import (
    ACK, ARM, CLEAR, CONTROL, DISARM, ESTOP, HELLO, KIND_NAMES, MAX_TTL_MS,
    STATUS, WIFI_BENCH_LEASE_MS, WIFI_BENCH_TTL_MS, Frame, FlapCommand, Status,
    control_payload, decode_or_none,
)

CONTROL_INTERVAL = 0.02
STATUS_TIMEOUT = 0.15
HELLO_TIMEOUT = 0.5
RETRY_INTERVAL = 0.08
RETRY_LIMIT = 3
MAX_SEQUENCE = 0xFFFFFFFF - 32
ZERO_PAYLOAD = bytes(32)


@dataclass
class Event:
    at: float
    text: str


@dataclass
class Snapshot:
    state: str
    reason: str
    session: int
    sequence: int
    lease: int
    status_sequence: int
    status: Status | None
    status_age: float
    control_age: float
    tx: int
    rx: int
    bad: int
    rejected: int
    rejected_delta: int
    pending: dict[int, tuple[int, int, int]]
    control_ttl_ms: int = MAX_TTL_MS
    status_timeout_ms: int = 150


class RcSession:
    """一条遥控会话。

    ``state``：
      idle      未连接
      hello     已发 HELLO，等 STATUS
      connected 会话有效，可发 CONTROL/ARM
      closed    已关闭，必须显式 connect 才能恢复
    """

    def __init__(self, send: Callable[[bytes], bool],
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._send_bytes = send
        self._clock = clock
        self.state = "idle"
        self.reason = ""
        self.session = 0
        self.sequence = 0
        self.nonce = 0
        self.status_sequence = 0
        self.lease = 0
        self.lease_deadline = 0.0
        self.last_status = 0.0
        self.last_control = 0.0
        self.next_control = 0.0
        self.hello_deadline = 0.0
        self.control_ttl_ms = MAX_TTL_MS
        self.status_timeout = STATUS_TIMEOUT
        self.status: Status | None = None
        self.previous_rejected = 0
        self.rejected_delta = 0
        self.tx = 0
        self.rx = 0
        self.bad = 0
        self.last_ack: tuple[int, int, float] | None = None
        self.pending: dict[int, list] = {}
        self.events: deque[Event] = deque(maxlen=300)

    # -- helpers ---------------------------------------------------------

    def _event(self, text: str, now: float | None = None) -> None:
        self.events.append(Event(self._clock() if now is None else now, text))

    def _send(self, kind: int, payload: bytes = ZERO_PAYLOAD, *,
              session: int | None = None, request: int = 0) -> bool:
        if self.sequence >= MAX_SEQUENCE:
            self.close("序号接近上限，必须重新建立会话")
            return False
        self.sequence += 1
        frame = Frame(kind, self.session if session is None else session,
                      self.sequence, request, payload)
        try:
            raw = frame.encode()
        except ValueError:
            return False
        ok = bool(self._send_bytes(raw))
        if ok:
            self.tx += 1
        return ok

    @staticmethod
    def _random_id() -> int:
        return secrets.randbits(32) or 1

    # -- lifecycle -------------------------------------------------------

    def connect(self) -> bool:
        self.control_ttl_ms = MAX_TTL_MS
        self.status_timeout = STATUS_TIMEOUT
        self.state = "hello"
        self.reason = ""
        self.session = 0
        self.sequence = 0
        self.status_sequence = 0
        self.lease = 0
        self.lease_deadline = 0.0
        self.last_status = 0.0
        self.last_control = 0.0
        self.next_control = 0.0
        self.status = None
        self.pending.clear()
        self.rejected_delta = 0
        self.tx = self.rx = self.bad = 0
        self.nonce = self._random_id()
        now = self._clock()
        self.hello_deadline = now + HELLO_TIMEOUT
        self._event(f"发送 HELLO（nonce {self.nonce:08x}）", now)
        return self._send(HELLO, session=0, request=self.nonce)

    def close(self, reason: str) -> None:
        if self.state == "closed" and self.reason == reason:
            return
        self.state = "closed"
        self.reason = reason
        self.session = 0
        self.lease = 0
        self.lease_deadline = 0.0
        self.next_control = 0.0
        self.pending.clear()
        self._event(f"会话关闭：{reason}")

    def reset(self) -> None:
        self.control_ttl_ms = MAX_TTL_MS
        self.status_timeout = STATUS_TIMEOUT
        self.state = "idle"
        self.reason = ""
        self.session = 0
        self.sequence = 0
        self.nonce = 0
        self.status_sequence = 0
        self.lease = 0
        self.lease_deadline = 0.0
        self.last_status = 0.0
        self.last_control = 0.0
        self.next_control = 0.0
        self.status = None
        self.pending.clear()
        self.rejected_delta = 0
        self.tx = self.rx = self.bad = 0

    # -- receive ---------------------------------------------------------

    def handle(self, raw: bytes, now: float | None = None) -> None:
        now = self._clock() if now is None else now
        frame = decode_or_none(raw)
        if frame is None:
            self.bad += 1
            return
        self.rx += 1
        if frame.kind == STATUS:
            self._handle_status(frame, now)
        elif frame.kind == ACK:
            self._handle_ack(frame, now)

    def _handle_status(self, frame: Frame, now: float) -> None:
        if self.state == "hello":
            if frame.request != self.nonce or frame.session == 0:
                return
            try:
                status = Status.from_frame(frame)
            except ValueError:
                self.bad += 1
                return
            self.state = "connected"
            self.session = frame.session
            self.status_sequence = frame.sequence
            self.lease = status.lease
            self.lease_deadline = now + max(0.02, status.lease_ms / 1000.0)
            self.last_status = now
            self.next_control = now
            self.status = status
            self.previous_rejected = status.rejected
            self._event(f"会话建立：session {self.session:08x}", now)
            if status.lease_ms == WIFI_BENCH_LEASE_MS:
                self.control_ttl_ms = WIFI_BENCH_TTL_MS
                self.status_timeout = WIFI_BENCH_LEASE_MS / 1000.0
                self._event("台架 Wi-Fi：CONTROL TTL 300 ms，遥测超时 500 ms", now)
            return
        if self.state != "connected" or frame.session != self.session:
            return
        if frame.sequence <= self.status_sequence:
            return
        try:
            status = Status.from_frame(frame)
        except ValueError:
            self.bad += 1
            return
        self.status_sequence = frame.sequence
        self.lease = status.lease
        self.lease_deadline = now + max(0.02, status.lease_ms / 1000.0)
        self.last_status = now
        previous = self.status
        self.status = status
        if previous is not None:
            if status.armed and not previous.armed:
                self._event("飞控已解锁", now)
            elif previous.armed and not status.armed:
                self._event("飞控已退出解锁", now)
        self.rejected_delta = status.rejected - self.previous_rejected
        self.previous_rejected = status.rejected
        if self.rejected_delta > 0:
            self._event(f"飞控拒绝了 {self.rejected_delta} 帧", now)

    def _handle_ack(self, frame: Frame, now: float) -> None:
        if self.state != "connected" or frame.session != self.session:
            return
        entry = self.pending.get(frame.request)
        if entry is None:
            return
        kind = entry[0]
        if len(frame.payload) < 5 or frame.payload[4] != kind:
            self.bad += 1
            return
        result = struct.unpack_from("<I", frame.payload)[0]
        self.pending.pop(frame.request, None)
        self.last_ack = (kind, result, now)
        self._event(f"{KIND_NAMES.get(kind, kind)} {'成功' if result == 0 else '被拒绝'}", now)

    # -- periodic --------------------------------------------------------

    def tick(self, now: float | None = None, command: FlapCommand | None = None,
             allow_control: bool = True) -> None:
        now = self._clock() if now is None else now
        if self.state == "hello" and now > self.hello_deadline:
            self.close("HELLO 无应答（检查 IP/端口和飞控 Wi-Fi 入口）")
            return
        if self.state == "connected" and now - self.last_status > self.status_timeout:
            self.close(f"遥测超时（{round(self.status_timeout * 1000)} ms 未收到 STATUS）")
            return
        if self.state != "connected":
            return
        self._retry(now)
        if not allow_control or command is None or now < self.next_control:
            return
        if now >= self.lease_deadline:
            # lease 已失效；再发只会被飞控拒绝，等 STATUS 或超时关闭。
            return
        try:
            payload = control_payload(self.lease, command, ttl_ms=self.control_ttl_ms,
                                      bench_timing=self.control_ttl_ms == WIFI_BENCH_TTL_MS)
        except ValueError:
            self.bad += 1
            return
        if self._send(CONTROL, payload):
            self.last_control = now
        self.next_control += CONTROL_INTERVAL
        if self.next_control <= now:
            self.next_control = now + CONTROL_INTERVAL

    def _retry(self, now: float) -> None:
        for request, entry in list(self.pending.items()):
            kind, sent, attempts = entry
            if now - sent < RETRY_INTERVAL:
                continue
            if attempts >= RETRY_LIMIT:
                self.pending.pop(request, None)
                self._event(f"{KIND_NAMES.get(kind, kind)} ACK 超时", now)
                continue
            self._send(kind, session=self.session, request=request)
            entry[1] = now
            entry[2] = attempts + 1

    # -- commands --------------------------------------------------------

    def request(self, kind: int) -> bool:
        if self.state != "connected" or not self.session:
            return False
        request = self._random_id()
        if not self._send(kind, session=self.session, request=request):
            return False
        self.pending[request] = [kind, self._clock(), 0]
        return True

    def arm(self, now: float | None = None) -> tuple[bool, str]:
        now = self._clock() if now is None else now
        if self.state != "connected":
            return False, "未建立控制会话"
        if now - self.last_control > 0.1:
            return False, "控制帧不新鲜，先持续发送 CONTROL"
        if self.status is not None and self.status.armed:
            return False, "飞控已经解锁"
        if not self.request(ARM):
            return False, "ARM 发送失败"
        return True, "已发送 ARM，等待飞控确认"

    def disarm(self) -> tuple[bool, str]:
        if self.state != "connected":
            return False, "未建立控制会话"
        if not self.request(DISARM):
            return False, "DISARM 发送失败"
        return True, "已发送 DISARM"

    def clear(self) -> tuple[bool, str]:
        if self.state != "connected":
            return False, "未建立控制会话"
        if self.status is not None and self.status.armed:
            return False, "解锁状态下不允许清除故障"
        if not self.request(CLEAR):
            return False, "CLEAR 发送失败"
        return True, "已发送 CLEAR"

    def estop(self) -> None:
        # 急停不依赖会话：session=0 连发三次，然后本地关闭会话。
        for _ in range(3):
            self._send(ESTOP, session=0, request=0)
        self.close("已发送急停")

    # -- views -----------------------------------------------------------

    def snapshot(self, now: float | None = None) -> Snapshot:
        now = self._clock() if now is None else now
        return Snapshot(
            state=self.state,
            reason=self.reason,
            session=self.session,
            sequence=self.sequence,
            lease=self.lease,
            status_sequence=self.status_sequence,
            status=self.status,
            status_age=0.0 if not self.last_status else now - self.last_status,
            control_age=0.0 if not self.last_control else now - self.last_control,
            tx=self.tx,
            rx=self.rx,
            bad=self.bad,
            rejected=0 if self.status is None else self.status.rejected,
            rejected_delta=self.rejected_delta,
            pending={key: tuple(value) for key, value in self.pending.items()},
            control_ttl_ms=self.control_ttl_ms,
            status_timeout_ms=round(self.status_timeout * 1000),
        )
