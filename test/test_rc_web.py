import json
import pathlib
import queue
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import Mock, patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

from rc.config import AppConfig, load_config, save_config
from rc.mapping import ManualControls, Sticks, build_command
from rc.protocol import CONTROL
from rc.web import RcBridge, RcHttpServer, UI_TIMEOUT
from rc.worker import RcWorker
from test_rc_gui import FakeJoystick
from test_rc_loopback import FakeFlightController


class ConfigTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = pathlib.Path(directory.name) / "config" / "rc.json"
        self.enterContext(patch("rc.config.CONFIG_PATH", self.path))

    def test_save_creates_project_config_and_reloads(self):
        config = AppConfig(host="192.168.1.2", mapping={"trim_left_deg": 4.2})
        self.assertEqual(save_config(config), self.path)
        self.assertTrue(self.path.is_file())
        self.assertEqual(load_config(), config)

    def test_missing_config_returns_defaults(self):
        self.assertEqual(load_config(), AppConfig())

    def test_invalid_config_returns_defaults(self):
        self.path.parent.mkdir(parents=True)
        self.path.write_text("{not json", encoding="utf-8")
        self.assertEqual(load_config(), AppConfig())

    def test_write_errors_are_reported(self):
        with patch.object(pathlib.Path, "write_text", side_effect=PermissionError):
            with self.assertRaises(PermissionError):
                save_config(AppConfig())


class WebTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.config_path = pathlib.Path(directory.name) / "config" / "rc.json"
        self.enterContext(patch("rc.config.CONFIG_PATH", self.config_path))
        self.worker = Mock()
        self.worker.messages = queue.Queue()
        self.worker._connected = False
        self.bridge = RcBridge(AppConfig(), self.worker)
        self.server = RcHttpServer(self.bridge)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"
        self.addCleanup(self.cleanup)

    def cleanup(self):
        self.bridge.stop()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def request(self, path="/api/state", body=None, **extra_headers):
        headers = {"X-BF-Token": self.bridge.token, "X-BF-Client": "operator"}
        headers.update(extra_headers)
        raw = json.dumps(body).encode() if body is not None else None
        request = Request(self.base + path, raw, headers)
        try:
            with urlopen(request, timeout=2) as response:
                return response.status, json.loads(response.read())
        except HTTPError as error:
            with error:
                return error.code, json.loads(error.read())

    def test_state_does_not_expose_token_and_has_strict_json(self):
        status, data = self.request()
        self.assertEqual(status, 200)
        self.assertNotIn(self.bridge.token, json.dumps(data))
        json.dumps(data, allow_nan=False)
        self.assertIsNone(data["snapshot"])
        self.assertFalse(data["active"])

    def test_api_requires_token_and_rejects_foreign_origin(self):
        self.assertEqual(self.request(**{"X-BF-Token": "invalid"})[0], 403)
        self.assertEqual(self.request(**{"Origin": "https://example.com"})[0], 403)
        self.assertEqual(self.request(**{"Host": "example.com"})[0], 403)

    def test_connect_uses_existing_worker_and_rejects_invalid_port(self):
        status, _data = self.request("/api/action", {"action": "connect", "host": "127.0.0.1", "port": 3335})
        self.assertEqual(status, 200)
        self.worker.connect.assert_called_once_with("127.0.0.1", 3335)
        self.assertEqual(self.request("/api/action", {"action": "connect", "port": 0})[0], 400)

    def test_observer_cannot_control_but_can_estop(self):
        self.bridge.action("operator", {"action": "connect", "host": "127.0.0.1", "port": 3335})
        status, data = self.request(**{"X-BF-Client": "observer"})
        self.assertEqual(status, 200)
        self.assertTrue(data["observer"])
        self.assertEqual(self.request("/api/action", {"action": "disarm"}, **{"X-BF-Client": "observer"})[0], 400)
        self.assertEqual(self.request("/api/action", {"action": "estop"}, **{"X-BF-Client": "observer"})[0], 200)
        self.worker.estop.assert_called_once()

    def test_arm_requires_continuous_hold_and_cancels_on_release(self):
        now = 100.0
        with patch("rc.web.time.monotonic", side_effect=lambda: now):
            with self.assertRaises(ValueError):
                self.bridge.action("operator", {"action": "arm"})
            self.bridge.action("operator", {"action": "arm_hold"})
            now += 0.2
            self.bridge.action("operator", {"action": "arm_cancel"})
            now += 1.3
            with self.assertRaises(ValueError):
                self.bridge.action("operator", {"action": "arm"})
            self.bridge.action("operator", {"action": "arm_hold"})
            for _ in range(11):
                now += 0.12
                self.bridge.action("operator", {"action": "arm_hold"})
            self.bridge.action("operator", {"action": "arm"})
            self.worker.arm.assert_called_once()
            with self.assertRaises(ValueError):
                self.bridge.action("operator", {"action": "arm"})

    def test_hold_without_heartbeat_cannot_arm(self):
        with patch("rc.web.time.monotonic", return_value=100.0):
            self.bridge.action("operator", {"action": "arm_hold"})
        with patch("rc.web.time.monotonic", return_value=102.0):
            with self.assertRaises(ValueError):
                self.bridge.action("operator", {"action": "arm"})
        self.worker.arm.assert_not_called()

    def test_invalid_config_never_reaches_worker(self):
        invalid = [
            ("manual", {"mode": 2}),
            ("manual", {"frequency_hz": "3"}),
            ("manual", {"frequency_hz": float("nan")}),
            ("limits", {"max_frequency_hz": -1}),
            ("limits", {"min_deg": [90, 90]}),
            ("roles", {"throttle": -1}),
            ("roles", {"arm_button": 3, "estop_button": 3}),
            ("mapping", {"use_joystick": 1}),
            ("mapping", {"deadband": 1}),
            ("mapping", {"trim_left_deg": 90.1}),
            ("mapping", {"trim_right_deg": -90.1}),
            ("mapping", {"trim_left_deg": float("nan")}),
            ("mapping", {"trim_right_deg": float("inf")}),
            ("mapping", {"trim_left_deg": "3"}),
            ("mapping", {"trim_right_deg": True}),
        ]
        for group, values in invalid:
            with self.subTest(group=group, values=values):
                self.assertEqual(self.request("/api/action", {"action": "configure", "group": group, "values": values})[0], 400)
        self.worker.set_manual.assert_not_called()
        self.worker.set_mapping.assert_not_called()
        self.worker.set_limits.assert_not_called()
        self.worker.set_roles.assert_not_called()

    def test_manual_amplitude_updates_mapping_and_preserves_mode(self):
        self.worker.manual.mode = 1
        self.bridge.action("operator", {"action": "configure", "group": "manual", "values": {"amplitude_left_deg": 12}})
        self.assertEqual(self.bridge.manual.mode, 1)
        self.assertEqual(self.bridge.mapping.amplitude_left_deg, 12)
        self.worker.set_manual.assert_called_once()
        self.worker.set_mapping.assert_called_once()

    def test_startup_does_not_restore_flap_mode(self):
        bridge = RcBridge(AppConfig(manual={"mode": 2}), Mock())
        self.assertEqual(bridge.manual.mode, 0)

    def test_trim_save_and_reload(self):
        self.assertEqual(build_command(Sticks(connected=True), self.bridge.mapping,
                                       ManualControls()).center_deg, (0, 0))
        values = {"trim_left_deg": 4.2, "trim_right_deg": -3.1}
        status, _data = self.request("/api/action", {
            "action": "configure", "group": "mapping", "values": values,
        })
        self.assertEqual(status, 200)
        self.worker.set_mapping.assert_called_once_with(self.bridge.mapping)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "rc.json"
            with patch("rc.config.CONFIG_PATH", path):
                self.assertEqual(self.request("/api/action", {"action": "save"})[0], 200)
                restored = RcBridge(load_config(), Mock())
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["mapping"]["trim_left_deg"], 4.2)
        self.assertEqual(build_command(Sticks(connected=True), restored.mapping,
                                       restored.manual).center_deg, (4.2, -3.1))
        self.assertEqual(restored.manual.mode, 0)

    def test_trim_cannot_change_while_armed(self):
        self.bridge.telemetry = types.SimpleNamespace(
            snapshot=types.SimpleNamespace(status=types.SimpleNamespace(armed=True)))
        with self.assertRaises(ValueError):
            self.bridge.action("operator", {"action": "configure", "group": "mapping",
                                             "values": {"trim_left_deg": 5}})
        self.worker.set_mapping.assert_not_called()
        self.assertEqual(self.bridge.mapping.trim_left_deg, 0)

    def test_save_permission_denied_returns_error(self):
        original_write = pathlib.Path.write_text

        def write_text(path, *args, **kwargs):
            if path == self.config_path:
                raise PermissionError(13, "Permission denied", str(path))
            return original_write(path, *args, **kwargs)

        with patch.object(pathlib.Path, "write_text", write_text):
            status, data = self.request("/api/action", {"action": "save"})
        self.assertEqual(status, 400)
        self.assertIn("Permission denied", data["error"])
        self.assertFalse(self.config_path.exists())

    def test_frequency_limit_updates_full_throttle_mapping(self):
        status, _data = self.request("/api/action", {
            "action": "configure", "group": "limits",
            "values": {"max_frequency_hz": 5},
        })
        self.assertEqual(status, 200)
        self.assertEqual(self.bridge.limits.max_frequency_hz, 5)
        self.assertEqual(self.bridge.mapping.max_frequency_hz, 5)
        self.worker.set_limits.assert_called_once_with(self.bridge.limits)
        self.worker.set_mapping.assert_called_once_with(self.bridge.mapping)
        self.assertEqual(self.bridge.manual.amplitude_left_deg, 20)
        self.assertEqual(self.bridge.manual.amplitude_right_deg, 20)

    def test_browser_loss_requests_estop(self):
        self.worker._connected = True
        self.bridge.owner = "operator"
        self.bridge.last_seen = time.monotonic() - UI_TIMEOUT - 0.1
        self.bridge.start()
        deadline = time.monotonic() + 1
        while not self.worker.estop.called and time.monotonic() < deadline:
            time.sleep(0.01)
        self.worker.estop.assert_called_once()
        self.assertIsNone(self.bridge.owner)

    def test_demo_is_isolated_and_writes_nothing(self):
        bridge = RcBridge(AppConfig(), demo=True)
        self.assertIsNone(bridge.worker)
        data = bridge.state("preview")
        self.assertTrue(data["demo"])
        json.dumps(data, allow_nan=False)
        with self.assertRaises(ValueError):
            bridge.action("preview", {"action": "connect"})
        with self.assertRaises(ValueError):
            bridge.action("preview", {"action": "save"})

    def test_static_page_is_served_and_paths_are_bounded(self):
        with urlopen(self.base, timeout=2) as response:
            self.assertIn("遥控工作台", response.read().decode())
            self.assertIn("frame-ancestors 'none'", response.headers["Content-Security-Policy"])
        self.assertEqual(self.request("/../config.py")[0], 404)


class WebLoopbackTests(unittest.TestCase):
    def test_connect_hold_arm_flap_and_estop(self):
        controller = FakeFlightController()
        controller.start()
        worker = RcWorker(FakeJoystick())
        bridge = RcBridge(AppConfig(mapping={"use_joystick": False, "require_joystick": False}), worker)
        bridge.start()

        def wait_for(condition):
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                bridge.state("operator")
                if condition():
                    return True
                time.sleep(0.01)
            return False

        try:
            bridge.action("operator", {"action": "connect", "host": "127.0.0.1", "port": controller.port})
            self.assertTrue(wait_for(lambda: bridge.telemetry is not None and
                                    bridge.telemetry.snapshot.state == "connected"))
            self.assertTrue(wait_for(lambda: any(frame.kind == CONTROL for frame in controller.received)))
            for _ in range(12):
                bridge.action("operator", {"action": "arm_hold"})
                time.sleep(0.12)
            bridge.action("operator", {"action": "arm"})
            self.assertTrue(wait_for(lambda: bridge.telemetry.snapshot.status is not None and
                                    bridge.telemetry.snapshot.status.armed))
            bridge.action("operator", {"action": "configure", "group": "manual",
                                       "values": {"frequency_hz": 0.5, "amplitude_left_deg": 5,
                                                  "amplitude_right_deg": 5}})
            bridge.action("operator", {"action": "mode", "mode": 2})
            self.assertTrue(wait_for(lambda: any(frame.kind == CONTROL and frame.payload[0] == 2
                                                for frame in controller.received)))
            bridge.action("operator", {"action": "estop"})
            self.assertTrue(wait_for(lambda: controller.estop))
            self.assertFalse(controller.armed)
        finally:
            bridge.stop()
            controller.stop()


if __name__ == "__main__":
    unittest.main()
