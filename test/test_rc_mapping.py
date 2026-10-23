import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from rc.mapping import (  # noqa: E402
    MODE_FLAP, MODE_HOLD, MODE_MANUAL, ManualControls, MappingConfig, Sticks,
    arm_blockers, build_command, mode_allowed, shape, sticks_to_command,
)
from rc.protocol import FlapLimits  # noqa: E402


class MappingTests(unittest.TestCase):
    def test_shape(self):
        self.assertEqual(shape(0.0, 0.05, 0.3), 0.0)
        self.assertEqual(shape(0.04, 0.05, 0.3), 0.0)
        self.assertAlmostEqual(shape(1.0, 0.05, 0.3), 1.0)
        self.assertAlmostEqual(shape(-1.0, 0.05, 0.3), -1.0)
        self.assertLess(shape(0.5, 0.05, 0.5), 0.5)

    def test_hold_and_manual(self):
        config = MappingConfig(center_range_deg=30.0)
        sticks = Sticks(throttle=0.0, pitch=1.0, roll=0.0, connected=True)
        hold = sticks_to_command(sticks, config, MODE_HOLD)
        self.assertEqual(hold.mode, MODE_HOLD)
        self.assertAlmostEqual(hold.center_deg[0], 30.0)
        self.assertAlmostEqual(hold.center_deg[1], 30.0)
        self.assertEqual(hold.frequency_hz, 0.0)
        manual = sticks_to_command(sticks, config, MODE_MANUAL)
        self.assertEqual(manual.mode, MODE_MANUAL)

    def test_flap_mapping(self):
        config = MappingConfig(max_frequency_hz=1.2, center_range_deg=20.0,
                               phase_range_deg=90.0, differential_range=0.5,
                               amplitude_deg=15.0, deadband=0.0, expo=0.0)
        sticks = Sticks(throttle=0.5, pitch=-0.5, roll=0.5, yaw=1.0, connected=True)
        command = sticks_to_command(sticks, config, MODE_FLAP)
        self.assertEqual(command.mode, MODE_FLAP)
        self.assertAlmostEqual(command.frequency_hz, 0.6)
        self.assertAlmostEqual(command.center_deg[0], -10.0)
        self.assertAlmostEqual(command.differential, 0.25)
        self.assertAlmostEqual(command.phase_deg, 90.0)
        self.assertEqual(command.amplitude_deg, (15.0, 15.0))

    def test_build_command_falls_back(self):
        config = MappingConfig(use_joystick=True)
        manual = ManualControls(mode=MODE_HOLD, center_left_deg=3.0)
        command = build_command(Sticks(connected=False), config, manual)
        self.assertEqual(command.center_deg, (3.0, 0.0))

    def test_arm_blockers(self):
        base = dict(connected=True, session=True, state=1, armed=False, faults=0,
                    throttle=0.0, joystick_connected=True, require_joystick=True,
                    mode=MODE_HOLD)
        self.assertEqual(arm_blockers(**base), [])
        self.assertTrue(arm_blockers(**dict(base, throttle=0.5)))
        self.assertTrue(arm_blockers(**dict(base, faults=1)))
        self.assertTrue(arm_blockers(**dict(base, state=0)))
        self.assertTrue(arm_blockers(**dict(base, joystick_connected=False)))
        self.assertTrue(arm_blockers(**dict(base, mode=MODE_FLAP)))

    def test_mode_allowed(self):
        self.assertTrue(mode_allowed(False, MODE_HOLD)[0])
        self.assertTrue(mode_allowed(True, MODE_HOLD)[0])
        self.assertFalse(mode_allowed(False, MODE_FLAP)[0])
        self.assertTrue(mode_allowed(True, MODE_FLAP)[0])


if __name__ == "__main__":
    unittest.main()
