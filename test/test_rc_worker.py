import pathlib
import sys
import threading
import time
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from rc.mapping import MappingConfig, ManualControls, Sticks  # noqa: E402
from rc.protocol import CONTROL, CONTROL_PAYLOAD  # noqa: E402
from rc.worker import RcWorker  # noqa: E402
from test_rc_gui import FakeJoystick  # noqa: E402
from test_rc_loopback import FakeFlightController  # noqa: E402


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.controller = FakeFlightController()
        self.controller.start()
        self.worker = RcWorker(FakeJoystick())
        self.worker.mapping = MappingConfig(use_joystick=False, require_joystick=False)
        self.worker.manual = ManualControls()
        self.worker.start()
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        try:
            self.worker.stop()
        finally:
            self.controller.stop()

    def wait_for(self, condition, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while True:
                try:
                    message = self.worker.messages.get_nowait()
                except Exception:
                    break
                if message.kind == "telemetry":
                    self.last = message.data
            if condition():
                return True
            time.sleep(0.01)
        return False

    def connect_and_arm(self):
        self.last = None
        self.worker.connect("127.0.0.1", self.controller.port)
        self.assertTrue(self.wait_for(
            lambda: self.last is not None and self.last.snapshot.state == "connected"))
        self.assertTrue(self.wait_for(
            lambda: any(frame.kind == CONTROL for frame in self.controller.received)))
        self.worker.arm()
        self.assertTrue(self.wait_for(
            lambda: self.last.snapshot.status is not None and self.last.snapshot.status.armed))

    def test_connect_and_arm(self):
        self.connect_and_arm()
        self.worker.estop()
        self.assertTrue(self.wait_for(lambda: self.controller.estop))

    def test_disarmed_sends_hold(self):
        self.last = None
        self.worker.connect("127.0.0.1", self.controller.port)
        self.assertTrue(self.wait_for(
            lambda: self.last is not None and self.last.snapshot.state == "connected"))
        # 配置成扑翼模式，但未解锁时实际命令仍必须是保持中位。
        self.worker.set_manual(ManualControls(mode=2, frequency_hz=0.8,
                                              amplitude_left_deg=20,
                                              amplitude_right_deg=20))
        self.assertTrue(self.wait_for(lambda: self.last.command.mode == 0))
        self.assertEqual(self.last.command.frequency_hz, 0.0)
        self.assertEqual(self.last.command.amplitude_deg, (0.0, 0.0))

    def test_manual_flap_without_joystick(self):
        self.connect_and_arm()
        self.worker.set_manual(ManualControls(mode=2, frequency_hz=0.5,
                                              amplitude_left_deg=5.0,
                                              amplitude_right_deg=5.0))
        self.assertTrue(self.wait_for(lambda: any(
            frame.kind == CONTROL and frame.payload[0] == 2
            for frame in self.controller.received)))
        frame = next(frame for frame in reversed(self.controller.received)
                     if frame.kind == CONTROL and frame.payload[0] == 2)
        payload = CONTROL_PAYLOAD.unpack(frame.payload)
        self.assertEqual(payload[6:9], (500, 500, 500))
        self.assertFalse(self.controller.estop)

    def test_trim_reaches_control_frame_after_arm(self):
        self.worker.set_mapping(MappingConfig(use_joystick=False, require_joystick=False,
                                              trim_left_deg=4.2, trim_right_deg=-3.1))
        self.connect_and_arm()
        self.worker.set_manual(ManualControls(mode=2, frequency_hz=0.5,
                                              amplitude_left_deg=5, amplitude_right_deg=5,
                                              center_left_deg=1, center_right_deg=-2))
        self.assertTrue(self.wait_for(lambda: any(
            frame.kind == CONTROL and frame.payload[0] == 2
            for frame in self.controller.received)))
        frame = next(frame for frame in reversed(self.controller.received)
                     if frame.kind == CONTROL and frame.payload[0] == 2)
        payload = CONTROL_PAYLOAD.unpack(frame.payload)
        self.assertEqual(payload[9:11], (520, -510))
        self.assertEqual(payload[12:14], (0, 0))

    def test_unused_joystick_does_not_block_manual_arm_or_trigger_estop(self):
        self.worker.joystick._sticks = Sticks(connected=True, throttle=1.0)
        self.connect_and_arm()
        self.worker.joystick._sticks = Sticks()
        self.assertTrue(self.wait_for(lambda: not self.last.sticks.connected))
        self.assertTrue(self.controller.armed)
        self.assertFalse(self.controller.estop)

    def test_joystick_loss_requires_explicit_reconnect(self):
        self.worker.joystick._sticks = Sticks(connected=True)
        self.worker.set_mapping(MappingConfig())
        self.connect_and_arm()
        self.worker.set_mode(2)
        self.worker.joystick._sticks = Sticks(connected=True, throttle=0.8)
        self.assertTrue(self.wait_for(lambda: any(
            frame.kind == CONTROL and frame.payload[0] == 2
            for frame in self.controller.received)))
        self.worker.joystick._sticks = Sticks()
        self.assertTrue(self.wait_for(lambda: self.controller.estop))
        self.assertTrue(self.wait_for(lambda: self.last.snapshot.state == "closed"))
        count = sum(frame.kind == CONTROL for frame in self.controller.received)
        self.worker.joystick._sticks = Sticks(connected=True, throttle=0.8)
        self.assertTrue(self.wait_for(lambda: self.last.sticks.connected))
        self.assertEqual(count, sum(frame.kind == CONTROL for frame in self.controller.received))
        self.assertFalse(self.controller.armed)
        self.assertFalse(self.last.active)
        self.assertEqual(self.last.command.mode, 0)

    def test_scan_keeps_control_and_estop_responsive(self):
        started = threading.Event()
        release = threading.Event()

        def blocked_discover(*_args, **_kwargs):
            started.set()
            release.wait(timeout=3.0)
            return ["127.0.0.2"]

        self.connect_and_arm()
        with patch("rc.worker.discover", side_effect=blocked_discover) as discovery:
            try:
                self.worker.scan()
                self.assertTrue(started.wait(timeout=0.5))
                count = sum(frame.kind == CONTROL for frame in self.controller.received)
                self.assertTrue(self.wait_for(lambda: sum(
                    frame.kind == CONTROL for frame in self.controller.received) >= count + 3,
                    timeout=0.5))
                self.worker.scan()
                self.worker.estop()
                self.assertTrue(self.wait_for(lambda: self.controller.estop, timeout=0.5))
                self.assertEqual(discovery.call_count, 1)
            finally:
                release.set()
                self.wait_for(lambda: not self.worker._scan_in_progress)
        self.assertEqual(self.worker.host, "127.0.0.1")

    def test_scan_failure_keeps_worker_alive(self):
        self.connect_and_arm()
        with patch("rc.worker.discover", side_effect=OSError("discovery failed")) as discovery:
            self.worker.scan()
            self.assertTrue(self.wait_for(lambda: discovery.called and
                                         not self.worker._scan_in_progress))
        self.assertTrue(self.worker._thread.is_alive())
        self.assertEqual(self.last.snapshot.state, "connected")


class TrimSafetyTests(unittest.TestCase):
    def setUp(self):
        self.worker = RcWorker(FakeJoystick())
        self.worker.mapping = MappingConfig(trim_left_deg=6, trim_right_deg=-4)
        self.worker.manual = ManualControls(mode=2, frequency_hz=0.5)
        self.worker._connected = True
        self.worker.session = types.SimpleNamespace(
            state="connected", status=types.SimpleNamespace(armed=True), rejected_delta=0)

    def test_low_throttle_keeps_trim_without_flapping(self):
        command = self.worker._desired_command(Sticks(connected=True, throttle=0))
        self.assertEqual(command.center_deg, (6, -4))
        self.assertEqual(command.mode, 0)
        self.assertEqual(command.frequency_hz, 0)
        self.assertEqual(command.amplitude_deg, (0, 0))

    def test_disarmed_ignores_trim_and_sticks(self):
        self.worker.session.status.armed = False
        command = self.worker._desired_command(Sticks(connected=True, throttle=1, pitch=1))
        self.assertEqual(command.center_deg, (0, 0))
        self.assertEqual(command.mode, 0)
        self.assertEqual(command.amplitude_deg, (0, 0))

    def test_missing_joystick_uses_original_fallback(self):
        command = self.worker._desired_command(Sticks())
        self.assertEqual(command.center_deg, (0, 0))
        self.assertEqual(command.mode, 0)
        self.assertEqual(command.amplitude_deg, (0, 0))

    def test_rejected_frames_use_original_fallback(self):
        self.worker.session.rejected_delta = 1
        command = self.worker._desired_command(Sticks(connected=True, throttle=1))
        self.assertEqual(command.center_deg, (0, 0))
        self.assertEqual(command.mode, 0)
        self.assertEqual(command.amplitude_deg, (0, 0))


if __name__ == "__main__":
    unittest.main()
