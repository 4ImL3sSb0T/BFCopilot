import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from rc.protocol import (  # noqa: E402
    ACK, ARM, CONTROL, DISARM, ESTOP, HELLO, STATUS, STATUS_PAYLOAD, Frame, FlapCommand,
)
from rc.session import RcSession  # noqa: E402


class Clock:
    def __init__(self, now=1000.0):
        self.now = now

    def __call__(self):
        return self.now


def status_bytes(session, sequence, request, lease, *, state=1, armed=0,
                 faults=0, rejected=0, lease_ms=150):
    payload = STATUS_PAYLOAD.pack(lease, lease_ms, state, armed, faults, 7500, 100,
                                  0, 0, 0, 10, rejected)
    return Frame(STATUS, session, sequence, request, payload).encode()


def ack_bytes(session, request, kind, result=0):
    payload = struct.pack("<I", result) + bytes([kind]) + bytes(27)
    return Frame(ACK, session, 999, request, payload).encode()


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.sent = []
        self.session = RcSession(lambda raw: self.sent.append(raw) or True,
                                 clock=self.clock)

    def _connect(self, session_id=0xABCD, lease=0x1111, lease_ms=150):
        self.session.connect()
        hello = Frame.decode(self.sent[-1])
        self.assertEqual(hello.kind, HELLO)
        self.assertEqual(hello.session, 0)
        self.assertEqual(hello.request, self.session.nonce)
        self.clock.now += 0.01
        self.session.handle(status_bytes(session_id, 1, self.session.nonce, lease,
                                         lease_ms=lease_ms),
                            self.clock.now)
        self.assertEqual(self.session.state, "connected")
        self.assertEqual(self.session.session, session_id)
        return hello

    def test_hello_and_control(self):
        self._connect()
        self.session.tick(self.clock.now, FlapCommand())
        control = Frame.decode(self.sent[-1])
        self.assertEqual(control.kind, CONTROL)
        self.assertEqual(control.session, 0xABCD)
        self.assertEqual(control.sequence, 2)
        self.assertEqual(struct.unpack_from("<I", control.payload, 8)[0], 0x1111)
        self.assertEqual(struct.unpack_from("<H", control.payload, 2)[0], 100)

    def test_lease_refresh(self):
        self._connect()
        self.session.tick(self.clock.now, FlapCommand())
        first = Frame.decode(self.sent[-1])
        self.assertEqual(struct.unpack_from("<I", first.payload, 8)[0], 0x1111)
        self.clock.now += 0.03
        self.session.handle(status_bytes(0xABCD, 2, self.session.nonce, 0x2222),
                            self.clock.now)
        self.session.tick(self.clock.now, FlapCommand())
        second = Frame.decode(self.sent[-1])
        self.assertEqual(struct.unpack_from("<I", second.payload, 8)[0], 0x2222)
        self.assertGreater(second.sequence, first.sequence)

    def test_status_timeout(self):
        self._connect()
        self.clock.now += 0.2
        self.session.tick(self.clock.now, FlapCommand())
        self.assertEqual(self.session.state, "closed")
        self.assertIn("遥测超时", self.session.reason)

    def test_hello_timeout(self):
        self.session.connect()
        self.clock.now += 1.0
        self.session.tick(self.clock.now, FlapCommand())
        self.assertEqual(self.session.state, "closed")

    def test_bench_timing_negotiation_and_timeout(self):
        self._connect(lease_ms=500)
        snapshot = self.session.snapshot()
        self.assertEqual(snapshot.control_ttl_ms, 300)
        self.assertEqual(snapshot.status_timeout_ms, 500)
        self.clock.now += 0.2
        self.session.tick(self.clock.now, FlapCommand())
        self.assertEqual(self.session.state, "connected")
        control = Frame.decode(self.sent[-1])
        self.assertEqual(control.kind, CONTROL)
        self.assertEqual(struct.unpack_from("<H", control.payload, 2)[0], 300)
        self.clock.now += 0.301
        sent = len(self.sent)
        self.session.tick(self.clock.now, FlapCommand())
        self.assertEqual(self.session.state, "closed")
        self.assertIn("500 ms", self.session.reason)
        self.assertEqual(len(self.sent), sent)

    def test_bench_lease_expiry_stops_control(self):
        self._connect(lease_ms=500)
        self.session.tick(self.clock.now, FlapCommand())
        self.clock.now = self.session.lease_deadline
        sent = len(self.sent)
        self.session.tick(self.clock.now, FlapCommand())
        self.assertEqual(len(self.sent), sent)

    def test_timing_resets_on_reset_and_reconnect(self):
        self._connect(lease_ms=500)
        self.session.reset()
        self.assertEqual(self.session.snapshot().control_ttl_ms, 100)
        self.assertEqual(self.session.snapshot().status_timeout_ms, 150)
        self._connect(lease_ms=500)
        self._connect()
        self.assertEqual(self.session.snapshot().status_timeout_ms, 150)
        self.session.tick(self.clock.now, FlapCommand())
        self.assertEqual(struct.unpack_from("<H", Frame.decode(self.sent[-1]).payload, 2)[0], 100)

    def test_unmatched_status_cannot_select_bench_timing(self):
        self.session.connect()
        self.session.handle(status_bytes(0xABCD, 1, self.session.nonce ^ 1, 1,
                                         lease_ms=500), self.clock.now)
        self.assertEqual(self.session.state, "hello")
        self.assertEqual(self.session.snapshot().control_ttl_ms, 100)
        self.session.handle(status_bytes(0xABCD, 1, self.session.nonce, 1), self.clock.now)
        self.session.handle(status_bytes(0xABCD, 2, self.session.nonce, 2,
                                         lease_ms=500), self.clock.now)
        self.assertEqual(self.session.snapshot().control_ttl_ms, 100)

    def test_arm_and_ack(self):
        self._connect()
        self.session.tick(self.clock.now, FlapCommand())
        ok, text = self.session.arm(self.clock.now)
        self.assertTrue(ok, text)
        arm = Frame.decode(self.sent[-1])
        self.assertEqual(arm.kind, ARM)
        self.assertIn(arm.request, self.session.pending)
        self.session.handle(ack_bytes(0xABCD, arm.request, ARM), self.clock.now)
        self.assertNotIn(arm.request, self.session.pending)
        self.assertEqual(self.session.last_ack[0], ARM)
        self.assertEqual(self.session.last_ack[1], 0)

    def test_arm_requires_fresh_control(self):
        self._connect()
        ok, text = self.session.arm(self.clock.now)
        self.assertFalse(ok)
        self.assertIn("不新鲜", text)

    def test_request_retry_and_timeout(self):
        self._connect()
        self.session.tick(self.clock.now, FlapCommand())
        self.session.disarm()
        disarm = Frame.decode(self.sent[-1])
        self.assertEqual(disarm.kind, DISARM)
        request = disarm.request
        for _ in range(4):
            self.clock.now += 0.09
            self.session.handle(status_bytes(0xABCD, 2 + _, self.session.nonce, 0x1111),
                                self.clock.now)
            self.session.tick(self.clock.now, FlapCommand())
        self.assertNotIn(request, self.session.pending)

    def test_estop(self):
        self._connect()
        self.session.estop()
        kinds = [Frame.decode(raw).kind for raw in self.sent[-3:]]
        self.assertEqual(kinds, [ESTOP, ESTOP, ESTOP])
        self.assertEqual(self.session.state, "closed")

    def test_bad_frame(self):
        self._connect()
        bad = bytearray(status_bytes(0xABCD, 2, self.session.nonce, 0x1111))
        bad[30] ^= 1
        self.session.handle(bytes(bad), self.clock.now)
        self.assertEqual(self.session.bad, 1)

    def test_sequence_strictly_increasing(self):
        self._connect()
        for _ in range(3):
            self.clock.now += 0.03
            self.session.tick(self.clock.now, FlapCommand())
        controls = [Frame.decode(raw) for raw in self.sent if Frame.decode(raw).kind == CONTROL]
        sequences = [frame.sequence for frame in controls]
        self.assertEqual(sequences, sorted(set(sequences)))


if __name__ == "__main__":
    unittest.main()
