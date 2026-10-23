import pathlib
import socket
import struct
import sys
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from rc.protocol import (  # noqa: E402
    ACK, ARM, CONTROL, CLEAR, DISARM, ESTOP, HELLO, STATUS, STATUS_PAYLOAD,
    FlapCommand, Frame,
)
from rc.session import RcSession  # noqa: E402
from rc.transport import UdpTransport  # noqa: E402


class FakeFlightController(threading.Thread):
    """最小飞控：HELLO/STATUS/lease/ARM/ACK/ESTOP，跑在本地 UDP 上。"""

    def __init__(self):
        super().__init__(daemon=True)
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.01)
        self.port = self.socket.getsockname()[1]
        self.running = True
        self.peer = None
        self.session = 0
        self.sequence = 0
        self.lease = 0
        self.nonce = 0
        self.armed = False
        self.estop = False
        self.received = []
        self.next_status = 0.0

    def run(self):
        while self.running:
            now = time.monotonic()
            data = None
            try:
                data, address = self.socket.recvfrom(2048)
            except socket.timeout:
                pass
            if data is not None:
                self.peer = address
                try:
                    frame = Frame.decode(data)
                except ValueError:
                    continue
                self.received.append(frame)
                self._handle(frame)
            if self.peer is not None and now >= self.next_status:
                self.send_status()
                self.next_status = now + 0.05

    def _handle(self, frame):
        if frame.kind == HELLO:
            self.session = 0x12345678
            self.sequence = 0
            self.nonce = frame.request
            self.armed = False
            self.send_status()
        elif frame.kind == CONTROL:
            self.lease = struct.unpack_from("<I", frame.payload, 8)[0]
        elif frame.kind == ARM:
            self.armed = True
            self.send_ack(frame.request, ARM, 0)
            self.send_status()
        elif frame.kind == DISARM:
            self.armed = False
            self.send_ack(frame.request, DISARM, 0)
            self.send_status()
        elif frame.kind == CLEAR:
            self.send_ack(frame.request, CLEAR, 0)
        elif frame.kind == ESTOP:
            self.armed = False
            self.estop = True

    def send_status(self):
        self.sequence += 1
        payload = STATUS_PAYLOAD.pack(0x1000 + self.sequence, 150,
                                      2 if self.armed else 1, int(self.armed),
                                      0, 7400, 1000, 0, 0, 0, 5, 0)
        self._send(Frame(STATUS, self.session, self.sequence, self.nonce, payload))

    def send_ack(self, request, kind, result):
        payload = struct.pack("<I", result) + bytes([kind]) + bytes(27)
        self._send(Frame(ACK, self.session, self.sequence, request, payload))

    def _send(self, frame):
        if self.peer is not None:
            try:
                self.socket.sendto(frame.encode(), self.peer)
            except OSError:
                pass

    def stop(self):
        if not self.running and not self.is_alive():
            return
        self.running = False
        if self.is_alive():
            self.join(timeout=1.0)
        try:
            self.socket.close()
        except OSError:
            pass


class LoopbackTests(unittest.TestCase):
    def setUp(self):
        self.controller = FakeFlightController()
        self.controller.start()
        self.transport = UdpTransport()
        self.transport.open()
        self.transport.set_peer("127.0.0.1", self.controller.port)
        self.session = RcSession(self.transport.send)

    def tearDown(self):
        self.transport.close()
        self.controller.stop()

    def pump(self, seconds=1.0, condition=None):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            now = time.monotonic()
            for data, _address in self.transport.receive():
                self.session.handle(data, now)
            self.session.tick(now, FlapCommand())
            if condition is not None and condition():
                return True
            time.sleep(0.005)
        return condition is None or condition()

    def test_connect_control_and_arm(self):
        self.assertTrue(self.session.connect())
        self.assertTrue(self.pump(1.0, lambda: self.session.state == "connected"))
        self.assertTrue(self.pump(
            0.5, lambda: any(frame.kind == CONTROL for frame in self.controller.received)))
        controls = [frame for frame in self.controller.received if frame.kind == CONTROL]
        self.assertTrue(controls)
        self.assertGreater(self.controller.lease, 0)
        ok, text = self.session.arm(time.monotonic())
        self.assertTrue(ok, text)
        self.assertTrue(self.pump(1.0, lambda: self.session.last_ack is not None))
        self.assertEqual(self.session.last_ack[0], ARM)
        self.assertTrue(self.controller.armed)

    def test_telemetry_timeout_closes_session(self):
        self.session.connect()
        self.assertTrue(self.pump(1.0, lambda: self.session.state == "connected"))
        self.controller.stop()
        self.assertTrue(self.pump(1.0, lambda: self.session.state == "closed"))
        self.assertIn("遥测超时", self.session.reason)

    def test_estop_is_sent(self):
        self.session.connect()
        self.assertTrue(self.pump(0.5, lambda: self.session.state == "connected"))
        self.session.estop()
        self.assertTrue(self.pump(
            0.5, lambda: len([frame for frame in self.controller.received
                              if frame.kind == ESTOP]) >= 3))
        estops = [frame for frame in self.controller.received if frame.kind == ESTOP]
        self.assertEqual(len(estops), 3)
        self.assertEqual(self.session.state, "closed")


if __name__ == "__main__":
    unittest.main()
