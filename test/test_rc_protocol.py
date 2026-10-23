import math
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from rc.protocol import (  # noqa: E402
    CONTROL, CONTROL_PAYLOAD, ESTOP, STATUS, STATUS_PAYLOAD, FlapCommand, FlapLimits,
    Frame, cobs_decode, cobs_encode, control_payload, fault_names, sanitize, validate,
)


class ProtocolTests(unittest.TestCase):
    def wire_command(self, command):
        values = CONTROL_PAYLOAD.unpack(control_payload(1, command))
        return FlapCommand(
            mode=values[0], frequency_hz=values[6] / 1000.0,
            amplitude_deg=(values[7] / 100.0, values[8] / 100.0),
            center_deg=(values[9] / 100.0, values[10] / 100.0),
            phase_deg=values[11] / 100.0,
            trim_deg=(values[12] / 100.0, values[13] / 100.0),
            differential=values[14] / 1000.0,
        )

    def test_sizes(self):
        self.assertEqual(CONTROL_PAYLOAD.size, 32)
        self.assertEqual(STATUS_PAYLOAD.size, 32)
        self.assertEqual(len(Frame(CONTROL).encode()), 60)

    def test_roundtrip_and_corruption(self):
        frame = Frame(CONTROL, 0x1234, 7, 0x99, control_payload(0x55, FlapCommand(mode=2)))
        self.assertEqual(Frame.decode(frame.encode()), frame)
        raw = bytearray(frame.encode())
        raw[30] ^= 1
        with self.assertRaises(ValueError):
            Frame.decode(bytes(raw))
        with self.assertRaises(ValueError):
            Frame.decode(frame.encode()[:-1])

    def test_cobs(self):
        raw = bytes(range(256))
        self.assertNotIn(0, cobs_encode(raw))
        self.assertEqual(cobs_decode(cobs_encode(raw)), raw)

    def test_control_units(self):
        command = FlapCommand(mode=2, frequency_hz=1.5, amplitude_deg=(10.0, 20.0),
                              center_deg=(-5.0, 5.0), phase_deg=180.0,
                              differential=-0.5)
        payload = control_payload(789, command)
        self.assertEqual(struct.unpack_from("<I", payload, 12)[0], 1500)
        self.assertEqual(struct.unpack_from("<H", payload, 16)[0], 1000)
        self.assertEqual(struct.unpack_from("<h", payload, 20)[0], -500)
        self.assertEqual(struct.unpack_from("<h", payload, 30)[0], -500)
        self.assertEqual(payload[0], 2)
        self.assertEqual(struct.unpack_from("<H", payload, 2)[0], 100)

    def test_invalid_control(self):
        with self.assertRaises(ValueError):
            control_payload(1, FlapCommand(frequency_hz=float("nan")))
        with self.assertRaises(ValueError):
            control_payload(1, FlapCommand(), ttl_ms=0)
        with self.assertRaises(ValueError):
            control_payload(1, FlapCommand(), ttl_ms=100, age_ms=100)

    def test_status_decode(self):
        payload = STATUS_PAYLOAD.pack(0x11223344, 150, 1, 0, 0, 7500, -1,
                                      -100, 200, 50, 12, 3)
        frame = Frame(STATUS, 0xAA, 9, 0x77, payload)
        from rc.protocol import Status
        status = Status.from_frame(frame)
        self.assertEqual(status.lease, 0x11223344)
        self.assertEqual(status.state, 1)
        self.assertEqual(status.vbat_mv, 7500)
        self.assertIsNone(status.iservo_ma)
        self.assertEqual(status.left_cd, -100)
        self.assertEqual(status.phase_cd, 50)
        self.assertEqual(status.rejected, 3)

    def test_fault_names(self):
        self.assertEqual(fault_names(0), [])
        self.assertEqual(fault_names(1 | (1 << 1)), ["LINK_LOST", "ESTOP"])

    def test_validate_and_sanitize(self):
        limits = FlapLimits()
        self.assertTrue(validate(FlapCommand(), limits)[0])
        command = FlapCommand(mode=2, frequency_hz=1.0, amplitude_deg=(60.0, 60.0),
                              center_deg=(0.0, 0.0))
        self.assertFalse(validate(command, limits)[0])
        cleaned, issues = sanitize(command, limits)
        self.assertTrue(validate(cleaned, limits)[0])
        self.assertTrue(issues)
        self.assertLessEqual(cleaned.amplitude_deg[0], 10.0)
        hold = sanitize(FlapCommand(mode=0, frequency_hz=1.0, amplitude_deg=(5, 5)),
                        limits)[0]
        self.assertEqual(hold.frequency_hz, 0.0)
        self.assertEqual(hold.amplitude_deg, (0.0, 0.0))

    def test_estop_constant(self):
        self.assertEqual(ESTOP, 5)

    def test_limited_flap_survives_wire_roundtrip(self):
        limits = FlapLimits()
        command, issues = sanitize(
            FlapCommand(mode=2, frequency_hz=1.0, amplitude_deg=(20.0, 20.0)), limits)
        decoded = self.wire_command(command)
        self.assertEqual(command.mode, 2)
        self.assertGreater(command.amplitude_deg[0], 9.0)
        self.assertTrue(issues)
        self.assertEqual(command, decoded)
        self.assertTrue(validate(decoded, limits)[0])
        self.assertLessEqual(decoded.amplitude_deg[0] * (2 * math.pi) ** 2, 360.0)

    def test_wire_precision_respects_frequency_travel_and_differential_limits(self):
        cases = [
            (FlapCommand(mode=2, frequency_hz=0.8006, amplitude_deg=(60.0, 60.0),
                         center_deg=(10.004, -12.006), differential=0.3336), FlapLimits()),
            (FlapCommand(mode=2, frequency_hz=1.0, amplitude_deg=(60.0, 60.0)),
             FlapLimits(max_frequency_hz=0.9996)),
            (FlapCommand(mode=2, frequency_hz=0.5004, amplitude_deg=(60.0, 60.0),
                         center_deg=(100.0, -100.0)),
             FlapLimits(min_deg=(-20.005, -25.005), max_deg=(20.005, 25.005))),
            (FlapCommand(mode=2, frequency_hz=0.5, amplitude_deg=(60.0, 60.0),
                         differential=-0.9996), FlapLimits(speed_dps=(10.0, 10.0))),
            (FlapCommand(mode=1, center_deg=(100.0, -100.0)),
             FlapLimits(min_deg=(-20.005, -25.005), max_deg=(20.005, 25.005))),
        ]
        for requested, limits in cases:
            with self.subTest(requested=requested, limits=limits):
                command, _issues = sanitize(requested, limits)
                decoded = self.wire_command(command)
                self.assertEqual(command.mode, requested.mode)
                self.assertEqual(command, decoded)
                self.assertEqual(command, sanitize(command, limits)[0])
                self.assertTrue(validate(decoded, limits)[0])
                self.assertLessEqual(decoded.frequency_hz, limits.max_frequency_hz)
                for side in range(2):
                    center = decoded.effective_center(side)
                    peak = decoded.effective_amplitude(side)
                    omega = 2 * math.pi * decoded.frequency_hz
                    self.assertGreaterEqual(center - peak, limits.min_deg[side])
                    self.assertLessEqual(center + peak, limits.max_deg[side])
                    self.assertLessEqual(peak * omega, limits.speed_dps[side])
                    self.assertLessEqual(peak * omega ** 2, limits.acceleration_dps2[side])

    def test_nonfinite_trim_is_rejected(self):
        for value in (float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                control_payload(1, FlapCommand(trim_deg=(value, 0.0)))


if __name__ == "__main__":
    unittest.main()
