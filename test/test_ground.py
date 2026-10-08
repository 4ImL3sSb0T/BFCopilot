import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from bf_ground import Frame, CONTROL, CONTROL_PAYLOAD, STATUS_PAYLOAD, cobs_encode, cobs_decode, control_payload


class GroundProtocolTests(unittest.TestCase):
    def test_sizes(self):
        self.assertEqual(CONTROL_PAYLOAD.size, 32)
        self.assertEqual(STATUS_PAYLOAD.size, 32)

    def test_roundtrip(self):
        frame = Frame(CONTROL, 123, 456, payload=control_payload(789))
        self.assertEqual(Frame.decode(frame.encode()), frame)
        self.assertEqual(cobs_decode(cobs_encode(frame.encode())), frame.encode())
        self.assertEqual(len(frame.encode()), 60)

    def test_corruption(self):
        raw = Frame(CONTROL).encode()
        for index in range(len(raw)):
            damaged = bytearray(raw)
            damaged[index] ^= 1
            with self.assertRaises(ValueError):
                Frame.decode(damaged)

    def test_cobs_boundaries(self):
        for raw in (bytes(60), bytes(range(256)), bytes([1]) * 508):
            encoded = cobs_encode(raw)
            self.assertNotIn(0, encoded)
            self.assertEqual(cobs_decode(encoded), raw)
        for raw in (b"\x00", b"\x05\x01", b"\x02\x00"):
            with self.assertRaises(ValueError):
                cobs_decode(raw)

    def test_command_units(self):
        payload = control_payload(789, mode=2, frequency=1.5, left=10, right=20,
                                  center_left=-5, phase=180, differential=-0.5)
        self.assertEqual(struct.unpack_from("<I", payload, 12)[0], 1500)
        self.assertEqual(struct.unpack_from("<h", payload, 20)[0], -500)
        self.assertEqual(struct.unpack_from("<h", payload, 30)[0], -500)
        with self.assertRaises(ValueError):
            control_payload(789, frequency=float("nan"))


if __name__ == "__main__":
    unittest.main()
