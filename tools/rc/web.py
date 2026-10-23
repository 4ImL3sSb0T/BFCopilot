"""本机浏览器遥控台，复用 RcWorker 的 USB、UDP 与安全状态机。"""

from __future__ import annotations

import json
import math
import queue
import secrets
import threading
import time
import webbrowser
from collections import deque
from dataclasses import asdict, replace
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from .config import (
    AppConfig, limits_from_dict, load_config, manual_from_dict, mapping_from_dict,
    roles_from_dict, save_config,
)
from .mapping import Sticks
from .protocol import FlapCommand, Status, fault_labels
from .worker import RcWorker

ASSETS = Path(__file__).with_name("web")
UI_TIMEOUT = 1.5


def json_safe(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {str(key): json_safe(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [json_safe(item) for item in value]
    return value


class RcBridge:
    def __init__(self, config: AppConfig | None = None, worker=None, *, demo=False):
        self.demo = demo
        self.worker = None if demo else (worker or RcWorker())
        config = config or load_config()
        self.mapping = mapping_from_dict(config.mapping)
        self.manual = replace(manual_from_dict(config.manual), mode=0)
        self.roles = roles_from_dict(config.roles)
        self.limits = limits_from_dict(config.limits)
        self.mapping.amplitude_left_deg = self.manual.amplitude_left_deg
        self.mapping.amplitude_right_deg = self.manual.amplitude_right_deg
        self.host, self.port = config.host, config.port
        self.token = secrets.token_urlsafe(32)
        self.lock = threading.RLock()
        self.logs = deque(maxlen=180)
        self.revision = 0
        self.telemetry = None
        self.telemetry_received_at = 0.0
        self.owner = None
        self.last_seen = 0.0
        self.hold_since = None
        self.hold_seen = 0.0
        self.started_at = time.monotonic()
        self.stop_event = threading.Event()
        self.thread = None
        if self.worker:
            self.worker.mapping, self.worker.manual = self.mapping, self.manual
            self.worker.roles, self.worker.limits = self.roles, self.limits
            self.worker.host, self.worker.port = self.host, self.port

    def start(self):
        if self.worker:
            self.worker.start()
        self.thread = threading.Thread(target=self._run, name="rc-web", daemon=True)
        self.thread.start()

    def stop(self):
        if self.worker:
            self.worker.shutdown()
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        if self.worker:
            self.worker.stop()

    def _run(self):
        while not self.stop_event.wait(0.02):
            with self.lock:
                if self.worker:
                    for _ in range(512):
                        try:
                            message = self.worker.messages.get_nowait()
                        except queue.Empty:
                            break
                        if message.kind == "telemetry":
                            self.telemetry = message.data
                            self.telemetry_received_at = time.monotonic()
                        elif message.kind == "log":
                            self._log(message.text)
                        elif message.kind == "discovered":
                            self.host = message.text
                            self.revision += 1
                            self._log(f"发现飞控 {message.text}")
                    if self.owner and time.monotonic() - self.last_seen > UI_TIMEOUT:
                        if self.worker._connected:
                            self.worker.estop()
                            self._log("浏览器连接中断，已请求急停；恢复后请重新连接")
                        self.owner = None
                        self.hold_since = None

    def _log(self, text):
        self.logs.append({"id": secrets.token_hex(4), "time": time.strftime("%H:%M:%S"),
                          "text": text})

    def config(self):
        return AppConfig(self.host, self.port, asdict(self.mapping), asdict(self.manual),
                         asdict(self.limits), asdict(self.roles))

    def state(self, client):
        with self.lock:
            if client == self.owner:
                self.last_seen = time.monotonic()
            data = self._demo_state() if self.demo else self._live_state()
            data.update(demo=self.demo, config=asdict(self.config()), revision=self.revision,
                        logs=list(self.logs), observer=bool(self.owner and client != self.owner),
                        bridge_ok=not self.worker or bool(self.worker._thread and
                                                         self.worker._thread.is_alive()))
            return json_safe(data)

    def _live_state(self):
        if self.telemetry is None:
            return {"snapshot": None, "sticks": asdict(Sticks()), "device": None,
                    "command": asdict(FlapCommand()), "blockers": ["等待摇杆采集"],
                    "issues": [], "active": False, "link_age": None, "mode": 0,
                    "command_ok": True, "fault_labels": []}
        data = asdict(self.telemetry)
        elapsed = max(0, time.monotonic() - self.telemetry_received_at)
        for key in ("status_age", "control_age"):
            data["snapshot"][key] += elapsed
        data["link_age"] += elapsed
        status = self.telemetry.snapshot.status
        data["fault_labels"] = fault_labels(status.faults) if status else []
        return data

    def _demo_state(self):
        elapsed = time.monotonic() - self.started_at
        left = 17 * math.sin(elapsed * 2.3)
        right = 14 * math.sin(elapsed * 2.3 + 0.4)
        sticks = Sticks(throttle=0.34 + 0.1 * math.sin(elapsed * 0.5),
                        yaw=0.18 * math.sin(elapsed * 0.7),
                        pitch=0.32 * math.sin(elapsed * 0.6),
                        roll=0.4 * math.cos(elapsed * 0.6), connected=True,
                        name="BETAFPV Joystick · 演示",
                        buttons=tuple(index == int(elapsed / 2) % 16 for index in range(16)))
        axes = [-1.0] * 8
        for name in ("yaw", "pitch", "roll"):
            axis = getattr(self.roles, name)
            if 0 <= axis < len(axes):
                axes[axis] = getattr(sticks, name) * (-1 if getattr(self.roles, "invert_" + name) else 1)
        axis = self.roles.throttle
        if 0 <= axis < len(axes):
            throttle = 1 - sticks.throttle if self.roles.invert_throttle else sticks.throttle
            axes[axis] = self.roles.throttle_low + throttle * (self.roles.throttle_high -
                                                             self.roles.throttle_low)
        axes[4:] = [-1.0, 0.0, 1.0, -1.0]
        sticks = replace(sticks, axes=tuple(axes))
        status = Status(1234, 142, 1, False, 0, int(4050 + 14 * math.sin(elapsed)),
                        int(340 + 85 * math.sin(elapsed * 2)), int(left * 100),
                        int(right * 100), int(elapsed * 1700) % 36000, 12, 0)
        return {"snapshot": {"state": "connected", "reason": "演示数据",
                             "session": 1843741, "sequence": int(elapsed * 50),
                             "lease": 1234, "status_sequence": int(elapsed * 20),
                             "status": asdict(status), "status_age": 0.012,
                             "control_age": 0.009, "tx": int(elapsed * 50),
                             "rx": int(elapsed * 20), "bad": 0, "rejected": 0,
                             "rejected_delta": 0, "pending": {}},
                "sticks": asdict(sticks),
                "device": {"connected": True, "name": sticks.name, "axes": 8,
                           "buttons": 16, "hats": 0, "error": ""},
                "command": asdict(FlapCommand(mode=0, center_deg=(left, right))),
                "blockers": ["演示模式不向设备发送任何控制"], "issues": [],
                "active": True, "mode": 0, "link_age": 0.012, "command_ok": True,
                "fault_labels": []}

    def action(self, client, body):
        if not client or len(client) > 128 or not isinstance(body, dict):
            raise ValueError("请求格式无效")
        action = body.get("action")
        with self.lock:
            if self.demo:
                raise ValueError("当前为演示模式，不执行设备操作或保存设置")
            if self.owner and self.owner != client and action != "estop":
                raise ValueError("另一个页面正在控制设备；请在原页面操作")
            allowed = {"connect", "disconnect", "scan", "arm_hold", "arm_cancel", "arm",
                       "disarm", "clear", "estop", "mode", "configure", "calibrate", "save"}
            if action not in allowed:
                raise ValueError("未知操作")
            if action == "configure":
                self._configure(body)
            elif action in ("connect", "scan"):
                host = body.get("host", self.host)
                port = body.get("port", self.port)
                if not isinstance(host, str) or len(host) > 253:
                    raise ValueError("飞控地址无效")
                if type(port) is not int or not 1 <= port <= 65535:
                    raise ValueError("端口必须在 1..65535")
                self.host, self.port = host.strip(), port
                if action == "connect":
                    self.worker.connect(self.host, self.port)
                else:
                    self.worker.scan(self.port)
                self.revision += 1
            elif action == "arm_hold":
                now = time.monotonic()
                if self.hold_since is None or now - self.hold_seen > 0.4:
                    self.hold_since = now
                self.hold_seen = now
            elif action == "arm_cancel":
                self.hold_since = None
            elif action == "arm":
                now = time.monotonic()
                if (self.hold_since is None or now - self.hold_since < 1.2 or
                        now - self.hold_seen > 0.4):
                    raise ValueError("请连续按住解锁按钮 1.2 秒")
                self.hold_since = None
                self.worker.arm()
            elif action == "mode":
                mode = body.get("mode")
                if type(mode) is not int or mode not in (0, 1, 2):
                    raise ValueError("模式无效")
                self.worker.set_mode(mode)
            elif action == "calibrate":
                if body.get("which") not in ("low", "high"):
                    raise ValueError("校准点无效")
                self.worker.calibrate(body["which"])
            elif action == "save":
                save_config(self.config())
                self._log("设置已保存到本机")
            else:
                getattr(self.worker, action)()
                if action in ("estop", "disconnect"):
                    self.hold_since = None
                    self.owner = None
                    return
            self.owner = client
            self.last_seen = time.monotonic()

    def _configure(self, body):
        group = body.get("group")
        values = body.get("values")
        if group not in ("mapping", "manual", "roles", "limits") or not isinstance(values, dict):
            raise ValueError("设置格式无效")
        if self.telemetry and self.telemetry.snapshot.status and self.telemetry.snapshot.status.armed:
            if group in ("roles", "limits", "mapping"):
                raise ValueError("请先上锁，再修改映射或限幅")
        current = asdict(getattr(self, group))
        if not values or any(key not in current for key in values):
            raise ValueError("设置字段无效")
        for key, value in values.items():
            if isinstance(value, (list, tuple)):
                if len(value) != 2 or any(type(item) not in (int, float) or
                                         not math.isfinite(item) for item in value):
                    raise ValueError("限幅必须是两个有限数值")
            elif type(current[key]) is bool:
                if type(value) is not bool:
                    raise ValueError("开关设置必须是布尔值")
            elif group == "roles" and (key in ("arm_button", "estop_button") or
                                       key in ("throttle", "yaw", "pitch", "roll")):
                optional = key in ("arm_button", "estop_button")
                if not (optional and value is None) and (type(value) is not int or
                                                        not 0 <= value <= 63):
                    raise ValueError("轴或按键编号必须是 0..63 的整数")
            elif type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError("设置必须是有限数值")
        current.update(values)
        if group == "manual":
            if "mode" in values:
                raise ValueError("请使用模式按钮切换模式")
            current["mode"] = self.worker.manual.mode
            if not (0 <= current["frequency_hz"] <= 50 and
                    0 <= current["amplitude_left_deg"] <= 180 and
                    0 <= current["amplitude_right_deg"] <= 180 and
                    abs(current["differential"]) <= 1 and
                    all(abs(current[key]) <= 180 for key in
                        ("center_left_deg", "center_right_deg", "phase_deg"))):
                raise ValueError("命令参数超出允许范围")
        elif group == "mapping":
            if not (0 <= current["deadband"] <= 0.9 and 0 <= current["expo"] <= 1 and
                    0 <= current["differential_range"] <= 1 and
                    0 < current["max_frequency_hz"] <= 50 and
                    0 <= current["center_range_deg"] <= 90 and
                    0 <= current["phase_range_deg"] <= 180):
                raise ValueError("映射参数超出允许范围")
        elif group == "roles":
            if current["throttle_high"] <= current["throttle_low"] + 0.05:
                raise ValueError("油门最高点必须至少高于最低点 0.05")
            if current["arm_button"] is not None and current["arm_button"] == current["estop_button"]:
                raise ValueError("解锁和急停必须分配给不同按键")
        elif group == "limits":
            if not (0 < current["max_frequency_hz"] <= 50 and 0 < current["ramp_s"] <= 60 and
                    all(-180 <= low < high <= 180 for low, high in
                        zip(current["min_deg"], current["max_deg"])) and
                    all(value > 0 for key in ("speed_dps", "acceleration_dps2")
                        for value in current[key])):
                raise ValueError("本地限幅无效")
        factory = {"manual": manual_from_dict, "mapping": mapping_from_dict,
                   "roles": roles_from_dict, "limits": limits_from_dict}[group]
        updated = factory(current)
        getattr(self.worker, "set_" + group)(updated)
        setattr(self, group, updated)
        if group == "manual":
            self.mapping = replace(self.mapping, amplitude_left_deg=updated.amplitude_left_deg,
                                   amplitude_right_deg=updated.amplitude_right_deg)
            self.worker.set_mapping(self.mapping)
        elif group == "limits":
            self.mapping = replace(self.mapping, max_frequency_hz=updated.max_frequency_hz)
            self.worker.set_mapping(self.mapping)
        self.revision += 1


class RcHttpServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, bridge, address=("127.0.0.1", 0)):
        self.bridge = bridge
        super().__init__(address, RcHandler)


class RcHandler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def _reply(self, status, body, content_type="application/json; charset=utf-8"):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; "
                         "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'")
        self.end_headers()
        try:
            self.wfile.write(body)
        except ConnectionError:
            pass

    def _authorized(self):
        expected_host = f"127.0.0.1:{self.server.server_port}"
        if self.headers.get("Host") != expected_host:
            self._reply(403, {"error": "请通过启动时的本机地址访问"})
            return False
        origin = self.headers.get("Origin")
        if origin and origin != "http://" + expected_host:
            self._reply(403, {"error": "请求来源无效"})
            return False
        if not secrets.compare_digest(self.headers.get("X-BF-Token", ""), self.server.bridge.token):
            self._reply(403, {"error": "访问凭据无效，请打开终端中的完整地址"})
            return False
        return True

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/api/state":
            if self._authorized():
                self._reply(200, self.server.bridge.state(self.headers.get("X-BF-Client", "")))
            return
        assets = {"/": ("index.html", "text/html; charset=utf-8"),
                  "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                  "/style.css": ("style.css", "text/css; charset=utf-8")}
        if path not in assets:
            self._reply(404, {"error": "页面不存在"})
            return
        filename, content_type = assets[path]
        self._reply(200, (ASSETS / filename).read_bytes(), content_type)

    def do_POST(self):
        if urlsplit(self.path).path != "/api/action":
            self._reply(404, {"error": "接口不存在"})
            return
        if not self._authorized():
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 16384:
                raise ValueError("请求大小无效")
            body = json.loads(self.rfile.read(length),
                              parse_constant=lambda _value: None)
            self.server.bridge.action(self.headers.get("X-BF-Client", ""), body)
            self._reply(200, {"ok": True})
        except (ValueError, TypeError, OverflowError, OSError) as error:
            self._reply(400, {"error": str(error)})


def serve(config=None, *, demo=False, port=0, open_browser=True):
    bridge = RcBridge(config, demo=demo)
    server = RcHttpServer(bridge, ("127.0.0.1", port))
    url = f"http://127.0.0.1:{server.server_port}/#{bridge.token}"
    print(f"BFCopilot: {url}", flush=True)
    print("Ctrl+C stops the console.", flush=True)
    try:
        bridge.start()
        if open_browser:
            webbrowser.open(url)
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        bridge.stop()
        server.server_close()
    return 0
