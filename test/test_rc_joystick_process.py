import ctypes
import os
import pathlib
import queue
import sys
import time
import unittest
from unittest.mock import patch

sys.path[:0] = [str(pathlib.Path(__file__).resolve().parents[1] / "tools"),
                str(pathlib.Path(__file__).resolve().parent)]

from rc.joystick import AxisRoles, DeviceInfo, JoystickProcess
from rc.mapping import MappingConfig, Sticks
from rc.protocol import CONTROL
from rc.worker import RcWorker
from test_rc_loopback import FakeFlightController


def fake_hid_process(connection, rate_hz):
    roles = connection.recv()
    try:
        while roles is not None:
            connection.send((Sticks(connected=True, timestamp=time.monotonic(),
                                    axes=(float(roles.throttle),)),
                             DeviceInfo(connected=True, guid=str(os.getpid()),
                                        error="blocking" if roles.throttle == 7 else "")))
            if roles.throttle == 7:
                if os.name == "nt":
                    ctypes.PyDLL("kernel32").Sleep(400)
                else:
                    time.sleep(0.4)
            if connection.poll(1.0 / rate_hz):
                roles = connection.recv()
    except (EOFError, OSError):
        pass
    finally:
        connection.close()


class JoystickProcessTests(unittest.TestCase):
    def wait_for(self, condition, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return True
            time.sleep(0.005)
        return False

    def start_reader(self):
        reader = JoystickProcess()
        self.addCleanup(reader.stop)
        with patch("rc.joystick._joystick_process", fake_hid_process):
            reader.start()
            self.assertTrue(self.wait_for(lambda: reader.snapshot().connected))
        return reader

    def test_separate_process_receives_axis_configuration(self):
        reader = self.start_reader()
        self.assertNotEqual(reader.info().guid, str(os.getpid()))
        reader.configure(AxisRoles(throttle=4))
        self.assertTrue(self.wait_for(lambda: reader.snapshot().axes == (4.0,)))
        reader.stop()
        self.assertFalse(reader.snapshot().connected)

    def test_stale_input_is_disconnected(self):
        reader = self.start_reader()
        reader.configure(AxisRoles(throttle=7))
        self.assertTrue(self.wait_for(lambda: reader.info().error == "blocking"))
        self.assertTrue(self.wait_for(lambda: not reader.snapshot().connected, timeout=0.3))
        self.assertEqual(reader.info().error, "摇杆输入超时")

    def test_hid_holding_gil_keeps_udp_control_alive(self):
        reader = self.start_reader()
        controller = FakeFlightController()
        controller.start()
        self.addCleanup(controller.stop)
        worker = RcWorker(reader)
        worker.mapping = MappingConfig(use_joystick=False, require_joystick=False)
        worker.start()
        self.addCleanup(worker.stop)
        worker.connect("127.0.0.1", controller.port)
        self.assertTrue(self.wait_for(lambda: worker.session.state == "connected"))
        reader.configure(AxisRoles(throttle=7))
        self.assertTrue(self.wait_for(lambda: reader.info().error == "blocking"))
        count = sum(frame.kind == CONTROL for frame in controller.received)
        deadline = time.monotonic() + 0.7
        while time.monotonic() < deadline:
            try:
                worker.messages.get(timeout=0.02)
            except queue.Empty:
                pass
            self.assertEqual(worker.session.state, "connected", worker.session.reason)
        self.assertGreater(sum(frame.kind == CONTROL for frame in controller.received), count + 10)

    def test_stale_hid_input_requests_estop(self):
        reader = self.start_reader()
        controller = FakeFlightController()
        controller.start()
        self.addCleanup(controller.stop)
        worker = RcWorker(reader)
        worker.start()
        self.addCleanup(worker.stop)
        worker.connect("127.0.0.1", controller.port)
        self.assertTrue(self.wait_for(lambda: worker.session.state == "connected"))
        self.assertTrue(self.wait_for(lambda: worker._joystick_seen_connected))
        reader.configure(AxisRoles(throttle=7))
        self.assertTrue(self.wait_for(lambda: controller.estop))
        self.assertEqual(worker.session.state, "closed")
        count = sum(frame.kind == CONTROL for frame in controller.received)
        self.assertTrue(self.wait_for(lambda: reader.snapshot().connected))
        self.assertEqual(sum(frame.kind == CONTROL for frame in controller.received), count)


if __name__ == "__main__":
    unittest.main()
