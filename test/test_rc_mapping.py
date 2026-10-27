import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from rc.mapping import (  # noqa: E402
    MODE_FLAP, MODE_HOLD, MODE_MANUAL, ManualControls, MappingConfig, Sticks,
    arm_blockers, build_command, mode_allowed, prepare, shape, sticks_to_command,
)
from rc.protocol import FlapLimits, validate  # noqa: E402


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

    def test_trim_at_stick_center_in_all_modes(self):
        config = MappingConfig(trim_left_deg=4.2, trim_right_deg=-3.1)
        for mode in (MODE_HOLD, MODE_MANUAL, MODE_FLAP):
            with self.subTest(mode=mode):
                command = build_command(Sticks(connected=True), config, ManualControls(mode=mode))
                self.assertEqual(command.center_deg, (4.2, -3.1))

    def test_trim_adds_once_to_joystick_and_manual_centers(self):
        config = MappingConfig(trim_left_deg=5, trim_right_deg=-3, deadband=0, expo=0)
        sticks = Sticks(connected=True, throttle=0.5, pitch=0.4, roll=-0.2)
        for mode in (MODE_HOLD, MODE_MANUAL, MODE_FLAP):
            manual = ManualControls(mode=mode, center_left_deg=12, center_right_deg=-8)
            for joystick in (False, True):
                with self.subTest(mode=mode, joystick=joystick):
                    base = sticks_to_command(sticks, config, mode) if joystick else manual.command()
                    command = build_command(sticks, config, manual, use_joystick=joystick)
                    self.assertEqual(command, base.with_centers(base.center_deg[0] + 5,
                                                              base.center_deg[1] - 3))
                    self.assertEqual(manual.center_left_deg, 12)

    def test_default_trim_preserves_existing_commands(self):
        config = MappingConfig()
        sticks = Sticks(connected=True, throttle=0.5, pitch=0.4, roll=-0.2)
        for mode in (MODE_HOLD, MODE_MANUAL, MODE_FLAP):
            manual = ManualControls(mode=mode, center_left_deg=3, center_right_deg=-2)
            self.assertEqual(build_command(sticks, config, manual), sticks_to_command(sticks, config, mode))
            self.assertEqual(build_command(sticks, config, manual, use_joystick=False), manual.command())

    def test_trim_reduces_flap_travel_and_respects_limits(self):
        config = MappingConfig(trim_left_deg=80, trim_right_deg=-85, use_joystick=False)
        manual = ManualControls(mode=MODE_FLAP, frequency_hz=0.5)
        command, issues = prepare(build_command(Sticks(), config, manual), FlapLimits())
        self.assertEqual(command.center_deg, (80, -85))
        self.assertLessEqual(command.amplitude_deg[0], 10)
        self.assertLessEqual(command.amplitude_deg[1], 5)
        self.assertTrue(issues)
        self.assertTrue(validate(command, FlapLimits())[0])
        manual.mode = MODE_MANUAL
        manual.center_left_deg, manual.center_right_deg = 30, -30
        command, _issues = prepare(build_command(Sticks(), config, manual), FlapLimits())
        self.assertEqual(command.center_deg, (90, -90))

    def test_mode_allowed(self):
        self.assertTrue(mode_allowed(False, MODE_HOLD)[0])
        self.assertTrue(mode_allowed(True, MODE_HOLD)[0])
        self.assertFalse(mode_allowed(False, MODE_FLAP)[0])
        self.assertTrue(mode_allowed(True, MODE_FLAP)[0])


if __name__ == "__main__":
    unittest.main()
