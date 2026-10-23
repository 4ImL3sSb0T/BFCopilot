"""遥控工作线程：UDP 会话 + 摇杆轮询 + 50 Hz 控制帧。

Tkinter 主线程只通过队列与这里交互；所有 socket、pygame 和会话状态都在本线程。
"""

from __future__ import annotations

import queue
import threading
import time
from dataclasses import dataclass, field

from .joystick import AxisRoles, DeviceInfo, JoystickReader
from .mapping import (
    MODE_FLAP, MODE_HOLD, ManualControls, MappingConfig, Sticks,
    arm_blockers, build_command, command_ok, mode_allowed, prepare,
)
from .protocol import DISCOVER_REPLY, FlapCommand, FlapLimits
from .session import RcSession, Snapshot
from .transport import UdpTransport, discover

PUBLISH_INTERVAL = 0.05


@dataclass
class Telemetry:
    snapshot: Snapshot
    sticks: Sticks
    device: DeviceInfo
    command: FlapCommand
    issues: list[str]
    command_ok: bool
    blockers: list[str]
    mode: int
    link_age: float
    active: bool


@dataclass
class Message:
    kind: str
    text: str = ""
    data: object = None


class RcWorker:
    def __init__(self, joystick: JoystickReader | None = None) -> None:
        self.commands: queue.Queue[tuple] = queue.Queue(maxsize=128)
        self.messages: queue.Queue[Message] = queue.Queue(maxsize=512)
        self.joystick = joystick or JoystickReader()
        self.transport = UdpTransport()
        self.session = RcSession(self.transport.send)
        self.mapping = MappingConfig()
        self.manual = ManualControls()
        self.limits = FlapLimits()
        self.roles = AxisRoles()
        self.host = ""
        self.port = 3335
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._connected = False
        self._last_command = FlapCommand()
        self._events_sent = 0
        self._last_publish = 0.0
        self._arm_button_latched = False
        self._arm_button_since = 0.0
        self._arm_button_seen_low = False
        self._estop_latched = False
        self._joystick_seen_connected = False
        self._scan_in_progress = False
        self._last_issues: list[str] = []

    # -- lifecycle -------------------------------------------------------

    def start(self) -> None:
        if self._thread is not None:
            return
        self.transport.open()
        self.joystick.configure(self.roles)
        self.joystick.start()
        self._thread = threading.Thread(target=self._run, name="rc-worker", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        thread, self._thread = self._thread, None
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=2.0)
        self.joystick.stop()
        self.transport.close()

    # -- UI API ----------------------------------------------------------

    def connect(self, host: str, port: int) -> None:
        self.commands.put(("connect", host, port))

    def disconnect(self) -> None:
        self.commands.put(("disconnect",))

    def arm(self) -> None:
        self.commands.put(("arm",))

    def disarm(self) -> None:
        self.commands.put(("disarm",))

    def clear(self) -> None:
        self.commands.put(("clear",))

    def estop(self) -> None:
        self.commands.put(("estop",))

    def shutdown(self) -> None:
        self.commands.put(("shutdown",))

    def set_mode(self, mode: int) -> None:
        self.commands.put(("mode", mode))

    def _post_config(self, *command) -> None:
        # 连续拖动滑块时丢一帧没关系，下一帧会带更新的值；不要卡住 Tk 主线程。
        try:
            self.commands.put_nowait(command)
        except queue.Full:
            pass

    def set_manual(self, manual: ManualControls) -> None:
        self._post_config("manual", manual)

    def set_mapping(self, mapping: MappingConfig) -> None:
        self._post_config("mapping", mapping)

    def set_limits(self, limits: FlapLimits) -> None:
        self._post_config("limits", limits)

    def set_roles(self, roles: AxisRoles) -> None:
        self._post_config("roles", roles)

    def set_peer(self, host: str, port: int) -> None:
        self.commands.put(("peer", host, port))

    def scan(self, port: int | None = None) -> None:
        self.commands.put(("scan", self.port if port is None else port))

    def calibrate(self, which: str) -> None:
        self.commands.put(("calibrate", which))

    # -- worker ----------------------------------------------------------

    def _run(self) -> None:
        next_tick = time.monotonic()
        while not self._stop.is_set():
            now = time.monotonic()
            self._handle_commands(now)
            if self._connected:
                for data, _address in self.transport.receive():
                    if data == DISCOVER_REPLY or data.startswith(DISCOVER_REPLY):
                        continue
                    self.session.handle(data, now)
            sticks = self.joystick.snapshot()
            self._handle_joystick_safety(sticks)
            self._maybe_arm_button(sticks, now)
            self._maybe_estop_button(sticks, now)
            command = self._desired_command(sticks)
            self._last_command = command
            self.session.tick(now, command, allow_control=self._connected)
            if self.session.state == "closed":
                self._connected = False
            self._publish(now, sticks, command)
            next_tick += 0.01
            delay = next_tick - time.monotonic()
            if delay > 0:
                self._stop.wait(min(delay, 0.02))
            else:
                next_tick = time.monotonic()

    def _handle_commands(self, now: float) -> None:
        for _ in range(32):
            try:
                command = self.commands.get_nowait()
            except queue.Empty:
                return
            action = command[0]
            if action == "connect":
                host, port = command[1], command[2]
                if not host:
                    found = discover(int(port), timeout=2.0)
                    if not found:
                        self._log("未发现飞控；确认电脑和飞控连在同一个热点")
                        continue
                    host = found[0]
                    self.messages.put(Message("discovered", host, found))
                try:
                    self.transport.set_peer(host, port)
                except ValueError as error:
                    self._log(f"连接参数无效：{error}")
                    continue
                self.host, self.port = host, int(port)
                self._connected = True
                self._estop_latched = False
                self._joystick_seen_connected = False
                self._arm_button_latched = False
                self._arm_button_seen_low = False
                self.session.connect()
                self._event_seen_reset()
                self._log(f"连接 {host}:{port}，发送 HELLO")
            elif action == "peer":
                try:
                    self.transport.set_peer(command[1], command[2])
                    self.host, self.port = command[1], int(command[2])
                except ValueError as error:
                    self._log(f"连接参数无效：{error}")
            elif action == "disconnect":
                self._graceful_stop("用户断开")
            elif action == "scan":
                port = int(command[1])
                if not 1 <= port <= 65535:
                    self._log("扫描端口必须在 1..65535")
                    continue
                if self._scan_in_progress:
                    self._log("正在扫描飞控")
                    continue
                self._scan_in_progress = True
                threading.Thread(target=self._scan_async, args=(port,),
                                 name="rc-discover", daemon=True).start()
            elif action == "scan_result":
                self._scan_in_progress = False
                found = command[1]
                if found:
                    self.messages.put(Message("discovered", found[0], found))
                else:
                    self.messages.put(Message("log", "未发现飞控；确认电脑和飞控连在同一个热点"))
            elif action == "mode":
                mode = int(command[1])
                status = self.session.status
                allowed, reason = mode_allowed(bool(status and status.armed), mode)
                if allowed:
                    self.manual.mode = mode
                else:
                    self._log(reason)
            elif action == "manual":
                manual = command[1]
                self.manual = manual
            elif action == "mapping":
                self.mapping = command[1]
            elif action == "limits":
                self.limits = command[1]
            elif action == "roles":
                self.roles = command[1]
                self.joystick.configure(self.roles)
            elif action == "calibrate":
                self._calibrate(str(command[1]))
            elif action == "arm":
                self._request_arm()
            elif action == "disarm":
                ok, text = self.session.disarm()
                self._log(text if not ok else "DISARM 已发送")
            elif action == "clear":
                ok, text = self.session.clear()
                self._log(text if not ok else "CLEAR 已发送")
            elif action == "estop":
                self._emergency_stop("界面急停")
            elif action == "shutdown":
                self._emergency_stop("关闭上位机")
                self._stop.set()
                return

    def _desired_command(self, sticks: Sticks) -> FlapCommand:
        status = self.session.status
        armed = self._connected and self.session.state == "connected" and bool(status and status.armed)
        selected = self.manual.mode
        issues: list[str] = []
        if self.mapping.use_joystick and not sticks.connected and self.mapping.require_joystick:
            command = FlapCommand()
            issues.append("摇杆未连接，保持中位")
        else:
            command = build_command(sticks, self.mapping, self.manual)
            if not armed:
                # 未解锁时永远只发保持中位；不会因为界面停在扑翼模式而起扑。
                command = FlapCommand()
            elif (selected == MODE_FLAP and self.mapping.use_joystick and sticks.connected
                  and sticks.throttle <= 0.05):
                # 油门在最低位时保持中心角，但不起扑。
                command = FlapCommand(mode=MODE_HOLD, center_deg=command.center_deg)
                issues.append("油门在最低位，暂不扑翼")
            if armed and self.session.rejected_delta > 0:
                # 飞控正在拒绝帧；立即退回保持中位，避免继续发无效命令。
                command = FlapCommand()
                issues.append("飞控拒绝了上一帧，已退回保持中位")
        prepared, prepared_issues = prepare(command, self.limits)
        self._last_issues = issues + prepared_issues
        return prepared

    def _handle_joystick_safety(self, sticks: Sticks) -> None:
        lost = self._joystick_seen_connected and not sticks.connected
        self._joystick_seen_connected = sticks.connected
        if self.mapping.use_joystick and lost and self._connected:
            self._emergency_stop("摇杆断开")

    def _scan_async(self, port: int) -> None:
        try:
            found = discover(port, timeout=1.0)
        except Exception as error:
            self._log(f"扫描失败：{error}")
            found = []
        try:
            self.commands.put_nowait(("scan_result", found))
        except queue.Full:
            self._scan_in_progress = False

    def _request_arm(self) -> None:
        now = time.monotonic()
        snapshot = self.session.snapshot(now)
        status = snapshot.status
        state = status.state if status else 0
        faults = status.faults if status else 0
        sticks = self.joystick.snapshot()
        blockers = arm_blockers(
            connected=self._connected, session=snapshot.state == "connected",
            state=state, armed=bool(status and status.armed), faults=faults,
            throttle=sticks.throttle if self.mapping.use_joystick and sticks.connected else 0.0,
            joystick_connected=sticks.connected,
            require_joystick=self.mapping.require_joystick and self.mapping.use_joystick,
            mode=self.manual.mode,
        )
        if blockers:
            self._log("解锁被阻止：" + "；".join(blockers))
            return
        # 先补一帧新鲜 CONTROL，再发 ARM。
        self.session.tick(now, self._last_command, allow_control=True)
        ok, text = self.session.arm(now)
        self._log(text if ok else f"解锁失败：{text}")

    def _maybe_arm_button(self, sticks: Sticks, now: float) -> None:
        index = self.roles.arm_button
        if index is None or not sticks.connected or index >= len(sticks.buttons):
            self._arm_button_latched = False
            self._arm_button_since = 0.0
            self._arm_button_seen_low = False
            return
        pressed = sticks.buttons[index]
        if not pressed:
            # 必须先看到一次松开，之后按住才算一次新的解锁动作；
            # 上电时开关已经停在解锁位不会自动解锁。
            self._arm_button_seen_low = True
            self._arm_button_latched = False
            self._arm_button_since = 0.0
            return
        if not self._arm_button_seen_low or self._arm_button_latched:
            return
        if not self._arm_button_since:
            self._arm_button_since = now
            return
        if now - self._arm_button_since >= 1.0:
            self._arm_button_latched = True
            self._request_arm()

    def _maybe_estop_button(self, sticks: Sticks, now: float) -> None:
        index = self.roles.estop_button
        if index is None or not sticks.connected or index >= len(sticks.buttons):
            self._estop_latched = False
            return
        if sticks.buttons[index]:
            if not self._estop_latched:
                self._estop_latched = True
                self._emergency_stop("摇杆急停键")
        else:
            self._estop_latched = False

    def _emergency_stop(self, reason: str) -> None:
        self.session.estop()
        self._connected = False
        self._log(f"已发送急停（{reason}），必须重新连接")

    def _graceful_stop(self, reason: str) -> None:
        status = self.session.status
        if status is not None and status.armed:
            self.session.disarm()
            # 给 DISARM 一点时间上链路；随后本地关闭，飞控仍会按 TTL 自行锁定。
            time.sleep(0.08)
        self.session.close(reason)
        self._connected = False
        self.transport.set_peer(self.host, self.port)
        self._log(reason)

    def _calibrate(self, which: str) -> None:
        sticks = self.joystick.snapshot()
        if not sticks.connected or self.roles.throttle >= len(sticks.axes):
            self._log("摇杆未连接，无法校准")
            return
        raw = sticks.axes[self.roles.throttle]
        if which == "low":
            self.roles.throttle_low = raw
            self._log(f"油门最低点 = {raw:+.3f}")
        elif which == "high":
            self.roles.throttle_high = raw
            self._log(f"油门最高点 = {raw:+.3f}")
        self.joystick.configure(self.roles)

    # -- output ----------------------------------------------------------

    def _event_seen_reset(self) -> None:
        self._events_sent = 0

    def _publish(self, now: float, sticks: Sticks, command: FlapCommand) -> None:
        events = list(self.session.events)
        if self._events_sent > len(events):
            self._events_sent = 0
        for event in events[self._events_sent:]:
            self.messages.put(Message("log", f"{time.strftime('%H:%M:%S')} {event.text}"))
        self._events_sent = len(events)
        if now - self._last_publish < PUBLISH_INTERVAL:
            return
        self._last_publish = now
        snapshot = self.session.snapshot(now)
        status = snapshot.status
        state = status.state if status else 0
        faults = status.faults if status else 0
        blockers = arm_blockers(
            connected=self._connected, session=snapshot.state == "connected",
            state=state, armed=bool(status and status.armed), faults=faults,
            throttle=sticks.throttle if self.mapping.use_joystick and sticks.connected else 0.0,
            joystick_connected=sticks.connected,
            require_joystick=self.mapping.require_joystick and self.mapping.use_joystick,
            mode=self.manual.mode,
        )
        ok, reason = command_ok(command, self.limits)
        telemetry = Telemetry(
            snapshot=snapshot,
            sticks=sticks,
            device=self.joystick.info(),
            command=command,
            issues=list(self._last_issues),
            command_ok=ok,
            blockers=blockers,
            mode=self.manual.mode,
            link_age=self.transport.age(),
            active=self._connected,
        )
        self.messages.put(Message("telemetry", "", telemetry))

    def _log(self, text: str) -> None:
        self.messages.put(Message("log", f"{time.strftime('%H:%M:%S')} {text}"))
