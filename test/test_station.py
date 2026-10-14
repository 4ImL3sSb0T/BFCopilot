import socket
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import MagicMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from station.link import Link, _sweep


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
            link = Link("127.0.0.1", server.port)
            with self.assertRaises(ConnectionError):
                link.connect()
            self.assertIsNone(link._socket)

    def test_timeout_closes_transport(self):
        link = Link("127.0.0.1")
        with patch.object(link, "_connect"), patch.object(link, "_read_reply", side_effect=TimeoutError):
            connection = Mock()
            link._socket = connection
            with self.assertRaises(TimeoutError):
                link.run("status")
            connection.close.assert_called_once()
            self.assertIsNone(link._socket)

    def test_stream_timeout_covers_maximum_firmware_duration(self):
        link = Link("127.0.0.1")
        with patch.object(link, "_connect"), patch.object(link, "_read_reply", return_value="done") as read:
            connection = Mock()
            link._socket = connection
            link.run("hw imu stream 30")
            read.assert_called_once_with(connection, 35)
            read.reset_mock()
            link.run("hw servo")
            read.assert_called_once_with(connection, 8)

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


if __name__ == "__main__":
    unittest.main()
