import gc
import pathlib
import queue
import sys
import time
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))


class FakeJoystick:
    def __init__(self):
        from rc.joystick import AxisRoles, DeviceInfo
        from rc.mapping import Sticks
        self.roles = AxisRoles()
        self._info = DeviceInfo()
        self._sticks = Sticks()

    def configure(self, roles):
        self.roles = roles

    def info(self):
        return self._info

    def snapshot(self):
        return self._sticks

    def start(self):
        pass

    def stop(self):
        pass


class FakeWorker:
    def __init__(self):
        from rc.joystick import AxisRoles
        from rc.mapping import ManualControls, MappingConfig
        from rc.protocol import FlapLimits
        self.messages = queue.Queue()
        self.commands = queue.Queue()
        self.joystick = FakeJoystick()
        self.mapping = MappingConfig()
        self.manual = ManualControls()
        self.limits = FlapLimits()
        self.roles = AxisRoles()
        self.session = types.SimpleNamespace(last_ack=None)
        self.host = "192.168.4.1"
        self.port = 3335
        self.calls = []

    def _record(self, name, *args):
        self.calls.append((name, args))

    def start(self):
        self._record("start")

    def stop(self):
        self._record("stop")

    def connect(self, host, port):
        self._record("connect", host, port)

    def disconnect(self):
        self._record("disconnect")

    def arm(self):
        self._record("arm")

    def disarm(self):
        self._record("disarm")

    def clear(self):
        self._record("clear")

    def estop(self):
        self._record("estop")

    def shutdown(self):
        self._record("shutdown")

    def set_mode(self, mode):
        self.manual.mode = mode
        self._record("mode", mode)

    def set_manual(self, manual):
        self.manual = manual

    def set_mapping(self, mapping):
        self.mapping = mapping

    def set_limits(self, limits):
        self.limits = limits

    def set_roles(self, roles):
        self.roles = roles
        self.joystick.configure(roles)

    def set_peer(self, host, port):
        self.host, self.port = host, port

    def scan(self, port):
        self._record("scan", port)

    def calibrate(self, which):
        self._record("calibrate", which)


class RcGuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import tkinter
        except ImportError as error:
            raise unittest.SkipTest(f"Tkinter unavailable: {error}")
        try:
            probe = tkinter.Tk()
            probe.withdraw()
            probe.destroy()
        except tkinter.TclError as error:
            raise unittest.SkipTest(f"Tcl/Tk unavailable: {error}")

    def setUp(self):
        from rc.gui import AppConfig, RcStation
        self.worker = FakeWorker()
        self.window = RcStation(self.worker, AppConfig())
        self.window.withdraw()
        self.addCleanup(self._cleanup)

    def _cleanup(self):
        try:
            self.window._finish_close()
        except Exception:
            pass
        self.window = None
        gc.collect()

    def test_constructs(self):
        self.assertEqual(self.window.title(), "BFCopilot · 遥控台")
        self.window.update()

    def test_telemetry_updates(self):
        from rc.joystick import DeviceInfo
        from rc.mapping import Sticks
        from rc.protocol import STATUS_PAYLOAD, FlapCommand, Frame, Status
        from rc.session import Snapshot
        from rc.worker import Telemetry

        payload = STATUS_PAYLOAD.pack(0x1234, 150, 2, 1, 0, 7500, 1200,
                                      100, -100, 90, 8, 2)
        status = Status.from_frame(Frame(7, 0xAA, 3, 0, payload))
        snapshot = Snapshot(state="connected", reason="", session=0xAA, sequence=10,
                            lease=0x1234, status_sequence=3, status=status,
                            status_age=0.01, control_age=0.01, tx=20, rx=10, bad=0,
                            rejected=2, rejected_delta=0, pending={})
        telemetry = Telemetry(
            snapshot=snapshot, sticks=Sticks(throttle=0.5, yaw=0.1, pitch=-0.2,
                                             roll=0.3, connected=True, name="BETAFPV"),
            device=DeviceInfo(connected=True, name="BETAFPV", axes=8, buttons=16),
            command=FlapCommand(mode=2, frequency_hz=0.5, amplitude_deg=(10, 10),
                                center_deg=(1, -1), phase_deg=5, differential=0.1),
            issues=[], command_ok=True, blockers=[], mode=2, link_age=0.01, active=True)
        self.window._apply_telemetry(telemetry)
        self.window.update()
        self.assertIn("mode=2", self.window.command_label.cget("text"))
        self.assertIn("已解锁", self.window.state_label.cget("text"))
        self.assertEqual(self.window.stick_vars["throttle"].get(), "0.50")

    def test_mode_and_slider_calls(self):
        self.window._select_mode(1)
        self.assertEqual(self.worker.manual.mode, 1)
        self.window.sliders["amplitude_left"].set(12.5)
        self.window._slider_changed("amplitude_left")
        self.assertAlmostEqual(self.worker.manual.amplitude_left_deg, 12.5)

    def test_connect_uses_widgets(self):
        self.window.host_var.set("10.0.0.2")
        self.window.port_var.set("4000")
        self.window._connect()
        deadline = time.monotonic() + 1
        while not any(call[0] == "connect" for call in self.worker.calls) and \
                time.monotonic() < deadline:
            self.window.update()
            time.sleep(0.01)
        self.assertIn(("connect", ("10.0.0.2", 4000)), self.worker.calls)

    def test_estop(self):
        self.window._estop()
        self.assertIn(("estop", ()), self.worker.calls)

    def test_trim_adjust_apply_save_and_reset(self):
        from rc.gui import mapping_from_dict
        self.window._adjust_trim("trim_left_deg", 0.1)
        self.window._trim_vars["trim_right_deg"].set("-2.5")
        self.assertEqual(self.worker.mapping.trim_left_deg, 0)
        with patch("rc.gui.save_config") as save:
            self.window._apply_trim(save=True)
            restored = mapping_from_dict(save.call_args.args[0].mapping)
        self.assertAlmostEqual(restored.trim_left_deg, 0.1)
        self.assertEqual(restored.trim_right_deg, -2.5)
        self.assertTrue(self.worker.mapping.use_joystick)
        self.window._reset_trim()
        self.assertEqual(self.worker.mapping.trim_right_deg, -2.5)
        self.window._apply_trim()
        self.assertEqual(self.worker.mapping.trim_left_deg, 0)
        self.assertEqual(self.worker.mapping.trim_right_deg, 0)

    def test_invalid_trim_does_not_change_mapping(self):
        for value in ("nan", "inf", "-inf", "90.1", "-90.1", "invalid", ""):
            with self.subTest(value=value):
                self.window._trim_vars["trim_left_deg"].set(value)
                self.window._trim_vars["trim_right_deg"].set("5")
                with patch("rc.gui.save_config") as save:
                    self.window._apply_trim(save=True)
                    save.assert_not_called()
                self.assertEqual(self.worker.mapping.trim_left_deg, 0)
                self.assertEqual(self.worker.mapping.trim_right_deg, 0)

    def test_trim_cannot_change_while_armed(self):
        self.window.telemetry = types.SimpleNamespace(
            snapshot=types.SimpleNamespace(status=types.SimpleNamespace(armed=True)))
        self.window._trim_vars["trim_left_deg"].set("5")
        self.window._apply_trim()
        self.assertEqual(self.worker.mapping.trim_left_deg, 0)


if __name__ == "__main__":
    unittest.main()
