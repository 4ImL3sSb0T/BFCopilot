import os
import pathlib
import sys
import time
import types
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

from rc.joystick import JoystickReader


class JoystickTests(unittest.TestCase):
    def test_hidden_window_keeps_reading_stick_movement(self):
        initial_axes = (0.0, 0.0, -0.85, 0.0)
        axes = list(initial_axes)
        background_enabled = False

        def init_display():
            nonlocal background_enabled
            background_enabled = os.environ.get("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS") == "1"

        device = Mock()
        device.get_name.return_value = "BETAFPV Joystick"
        device.get_guid.return_value = "test-guid"
        device.get_numaxes.return_value = 4
        device.get_numbuttons.return_value = 0
        device.get_numhats.return_value = 0
        device.get_axis.side_effect = lambda index: (
            axes[index] if background_enabled else initial_axes[index])
        pygame = types.SimpleNamespace(
            HIDDEN=128,
            display=Mock(init=Mock(side_effect=init_display)),
            joystick=Mock(get_count=Mock(return_value=1),
                          Joystick=Mock(return_value=device)),
            event=Mock(),
        )
        reader = JoystickReader()
        with patch.dict(sys.modules, {"pygame": pygame}), patch.dict(
                os.environ, {"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "0"}):
            try:
                reader.start()
                self.assertTrue(self.wait_for(lambda: reader.snapshot().axes == initial_axes))
                axes[:] = [0.6, -0.4, 1.0, -0.8]
                self.assertTrue(self.wait_for(lambda: reader.snapshot().axes == tuple(axes)))
                sticks = reader.snapshot()
                self.assertTrue(sticks.connected)
                self.assertEqual(sticks.throttle, 1.0)
                self.assertEqual(sticks.yaw, 0.6)
                self.assertEqual(sticks.pitch, -0.4)
                self.assertEqual(sticks.roll, -0.8)
            finally:
                reader.stop()

    def wait_for(self, condition, timeout=1.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return True
            time.sleep(0.01)
        return False


if __name__ == "__main__":
    unittest.main()
