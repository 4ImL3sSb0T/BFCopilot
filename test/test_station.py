import socket
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import MagicMock, Mock, patch

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from station.link import Link, _sweep, discover_serial


class ConsoleServer:
    def __init__(self, handler):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(3)
        self.port = self.listener.getsockname()[1]
        self.errors = []
        self.handler = handler
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        try:
            with self.listener:
                connection, _ = self.listener.accept()
                with connection:
                    connection.settimeout(2)
                    self.handler(connection)
        except Exception as error:
            self.errors.append(error)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.thread.join(4)
        if self.thread.is_alive():
            raise AssertionError("console server did not finish")
        if self.errors:
            raise self.errors[0]


class FakeSerial:
    """Serial console REPL double: echoes what it is sent, then appends canned output."""

    def __init__(self, respond=None):
        self.buffer = bytearray()
        self.writes = []
        self.respond = respond or (lambda data: b"")
        self.disconnected = False
        self.closed = False
        self.dtr = True
        self.rts = True

    def write(self, data):
        if self.disconnected:
            raise serial.SerialException("port closed")
        self.writes.append(bytes(data))
        self.buffer.extend(self.respond(bytes(data)))
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.buffer.clear()

    @property
    def in_waiting(self):
        return len(self.buffer)

    def read(self, size):
        if self.disconnected:
            raise serial.SerialException("port closed")
        chunk = bytes(self.buffer[:size])
        del self.buffer[:size]
        return chunk

    def close(self):
        self.closed = True

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class StationTransportTests(unittest.TestCase):
    def test_fragmented_prompt_and_utf8_output(self):
        def serve(connection):
            connection.sendall(b"bench\r\nhw")
            connection.sendall(b"> ")
            self.assertEqual(connection.recv(64), b"status\n")
            connection.sendall("状态正常\r\n".encode())
            connection.sendall(b"hw")
            connection.sendall(b"> ")

        with ConsoleServer(serve) as server:
            link = Link("127.0.0.1", server.port)
            self.addCleanup(link.close)
            self.assertEqual(link.run("status"), "状态正常\r\n")

    def test_eof_is_an_error_and_does_not_replay_command(self):
        received = []

        def serve(connection):
            connection.sendall(b"hw> ")
            received.append(connection.recv(64))
            connection.sendall(b"incomplete")

        with ConsoleServer(serve) as server:
            link = Link("127.0.0.1", server.port)
            with self.assertRaises(ConnectionError):
                link.run("hw pulse both 1500")
            self.assertIsNone(link._socket)
        self.assertEqual(received, [b"hw pulse both 1500\n"])

    def test_banner_requires_a_complete_prompt(self):
        def serve(connection):
            connection.sendall(b"some other TCP service")

        with ConsoleServer(serve) as server:
            link = Link("127.0.0.1", server.port, method="tcp")
            with self.assertRaises(ConnectionError):
                link.connect()
            self.assertIsNone(link._socket)

    def test_timeout_closes_transport(self):
        link = Link("127.0.0.1")
        with patch.object(link, "_connect"), patch.object(link, "_read_reply", side_effect=TimeoutError):
            connection = Mock()
            link._socket = connection
            link.transport = "tcp"
            with self.assertRaises(TimeoutError):
                link.run("status")
            connection.close.assert_called_once()
            self.assertIsNone(link._socket)

    def test_stream_timeout_covers_maximum_firmware_duration(self):
        link = Link("127.0.0.1")
        with patch.object(link, "_connect"), patch.object(link, "_read_reply", return_value="done") as read:
            connection = Mock()
            link._socket = connection
            link.transport = "tcp"
            link.run("hw imu stream 30")
            read.assert_called_once_with(connection, "tcp", 35)
            read.reset_mock()
            link.run("hw servo")
            read.assert_called_once_with(connection, "tcp", 8)

    def test_invalid_input_never_connects(self):
        link = Link("127.0.0.1")
        with patch.object(link, "_connect") as connect:
            for line in ("", "status\nestop", "status\r", "a\0b"):
                with self.subTest(line=line), self.assertRaises(ValueError):
                    link.run(line)
            connect.assert_not_called()

    def test_invalid_port_preserves_target(self):
        link = Link("original", 3333)
        for port in (0, 65536, "bad"):
            with self.assertRaises(ValueError):
                link.set_target("new", port)
        self.assertEqual((link.host, link.port), ("original", 3333))

    def test_discovery_ignores_an_unrelated_tcp_service(self):
        unrelated = MagicMock()
        unrelated.__enter__.return_value.recv.side_effect = [b"HTTP service", b""]
        board = MagicMock()
        board.__enter__.return_value.recv.side_effect = [b"hw", b"> "]
        with patch("station.link.local_prefixes", return_value=["127.0.0"]), patch(
                "station.link.socket.create_connection", side_effect=[unrelated, board]):
            self.assertEqual(_sweep(3333, timeout=0.5, workers=1), "127.0.0.2")

    def test_serial_reply_strips_the_repl_echo(self):
        connection = FakeSerial(lambda data: (
            b"\r\nhw> " if data == b"\n" else
            b"status\r\n" + "状态正常".encode() + b"\r\n" + b"hw" + b"> "))
        link = Link(method="serial", serial_port="COM9")
        with patch("station.link.open_serial_port", return_value=connection):
            self.assertEqual(link.run("status"), "状态正常\r\n")
        self.assertEqual(link.transport, "serial")
        self.assertEqual(link.describe(), "COM9（USB 串口）")

    def test_serial_disconnect_is_an_error_and_does_not_replay_command(self):
        connection = FakeSerial(lambda data: b"\r\nhw> ")

        def respond(data):
            if data == b"\n":
                return b"\r\nhw> "
            if data == b"hw pulse both 1500\n":
                connection.disconnected = True
            return b""

        connection.respond = respond
        link = Link(method="serial", serial_port="COM9")
        with patch("station.link.open_serial_port", return_value=connection):
            with self.assertRaises(ConnectionError):
                link.run("hw pulse both 1500")
        self.assertIsNone(link._serial)
        self.assertEqual(connection.writes, [b"\n", b"hw pulse both 1500\n"])

    def test_auto_falls_back_to_serial_when_wifi_is_unreachable(self):
        connection = FakeSerial(lambda data: b"\r\nhw> ")
        link = Link(host="192.0.2.1", method="auto")
        with patch.object(Link, "_connect_tcp", side_effect=TimeoutError("no route")), \
                patch("station.link.discover_serial", return_value="COM9"), \
                patch("station.link.open_serial_port", return_value=connection):
            self.assertEqual(link.connect(), "COM9（USB 串口）")
        self.assertEqual(link.transport, "serial")

    def test_auto_uses_tcp_when_it_answers(self):
        def serve(connection):
            connection.sendall(b"hw> ")

        with ConsoleServer(serve) as server:
            link = Link("127.0.0.1", server.port)
            self.addCleanup(link.close)
            self.assertEqual(link.connect(), f"127.0.0.1:{server.port}")
        self.assertEqual(link.transport, "tcp")

    def test_auto_reports_both_transports_when_neither_answers(self):
        link = Link(method="auto")
        with patch.object(Link, "_connect_tcp", side_effect=ValueError("no wifi")), \
                patch.object(Link, "_connect_serial", side_effect=ValueError("no serial")):
            with self.assertRaises(ValueError) as caught:
                link.connect()
        self.assertIn("Wi-Fi/TCP: no wifi", str(caught.exception))
        self.assertIn("串口: no serial", str(caught.exception))

    def test_serial_discovery_skips_bluetooth_ports(self):
        bluetooth = Mock(device="COM6", vid=None, description="Bluetooth link",
                         hwid="BTHENUM\\{0000}")
        board = Mock(device="COM9", vid=0x303A, description="USB JTAG/serial debug unit",
                     hwid="USB VID:PID=303A:1001")
        with patch("station.link.list_ports.comports", return_value=[bluetooth, board]), patch(
                "station.link._probe_serial", return_value=True) as probe:
            self.assertEqual(discover_serial(), "COM9")
        self.assertEqual([call.args[0] for call in probe.call_args_list], ["COM9"])


if __name__ == "__main__":
    unittest.main()
