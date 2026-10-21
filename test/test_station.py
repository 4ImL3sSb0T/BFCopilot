import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from station.link import Link, available_ports, discover


class FakeSerial:
    """Console REPL double: echoes what it is sent, then appends canned output."""

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


def prompt_for(command, output):
    def respond(data):
        if data == b"\n":
            return b"\r\nhw> "
        if data == command:
            return command + b"\r\n" + output + b"hw> "
        return b"hw> "

    return respond


class StationTransportTests(unittest.TestCase):
    def link_with(self, connection):
        link = Link("COM9")
        self.addCleanup(link.close)
        patcher = patch("station.link.open_port", return_value=connection)
        patcher.start()
        self.addCleanup(patcher.stop)
        return link

    def test_fragmented_prompt_utf8_output_and_echo_removal(self):
        connection = FakeSerial(lambda data: (
            b"\r\nhw> " if data == b"\n" else
            b"status\r\n" + "状态正常".encode() + b"\r\n" + b"hw" + b"> "))

        link = self.link_with(connection)
        self.assertEqual(link.run("status"), "状态正常\r\n")
        self.assertEqual(connection.writes, [b"\n", b"status\n"])

    def test_echo_removal_only_drops_the_first_matching_line(self):
        connection = FakeSerial(lambda data: (
            b"\r\nhw> " if data == b"\n" else b"status\r\n955\r\nstatus\r\nhw> "))

        link = self.link_with(connection)
        self.assertEqual(link.run("status"), "955\r\nstatus\r\n")

    def test_disconnect_is_an_error_and_does_not_replay_command(self):
        connection = FakeSerial(lambda data: b"\r\nhw> ")

        def respond(data):
            if data == b"\n":
                return b"\r\nhw> "
            if data == b"hw pulse both 1500\n":
                connection.disconnected = True
            return b""

        connection.respond = respond
        link = self.link_with(connection)
        with self.assertRaises(ConnectionError):
            link.run("hw pulse both 1500")
        self.assertIsNone(link._serial)
        self.assertEqual(connection.writes, [b"\n", b"hw pulse both 1500\n"])

    def test_timeout_closes_transport(self):
        link = Link("COM9")
        with patch.object(link, "_connect"), patch.object(link, "_read_reply",
                                                         side_effect=TimeoutError):
            connection = Mock()
            link._serial = connection
            with self.assertRaises(TimeoutError):
                link.run("status")
            connection.close.assert_called_once()
            self.assertIsNone(link._serial)

    def test_stream_timeout_covers_maximum_firmware_duration(self):
        link = Link("COM9")
        with patch.object(link, "_connect"), patch.object(
                link, "_read_reply", return_value="done") as read:
            connection = Mock()
            link._serial = connection
            link.run("hw imu stream 30")
            read.assert_called_once_with(connection, 35)
            read.reset_mock()
            link.run("hw servo")
            read.assert_called_once_with(connection, 8)

    def test_invalid_input_never_connects(self):
        link = Link("COM9")
        with patch.object(link, "_connect") as connect:
            for line in ("", "status\nestop", "status\r", "a\0b"):
                with self.subTest(line=line), self.assertRaises(ValueError):
                    link.run(line)
            connect.assert_not_called()

    def test_clearing_the_target_returns_to_auto_discovery(self):
        link = Link("COM9")
        link.set_target("  ")
        self.assertIsNone(link.port)
        link.set_target("COM7")
        self.assertEqual(link.port, "COM7")

    def test_bluetooth_ports_are_not_candidates(self):
        bluetooth = Mock(device="COM6", vid=None, description="Bluetooth link",
                         hwid="BTHENUM\\{0000}")
        board = Mock(device="COM9", vid=0x303A, description="USB JTAG/serial debug unit",
                     hwid="USB VID:PID=303A:1001")
        with patch("station.link.list_ports.comports", return_value=[bluetooth, board]):
            self.assertEqual(available_ports(), ["COM9"])

    def test_discovery_prefers_the_port_that_answers_with_a_prompt(self):
        with patch("station.link.available_ports", return_value=["COM1", "COM2"]), patch(
                "station.link._probe", side_effect=[False, True]) as probe:
            self.assertEqual(discover(), "COM2")
        self.assertEqual([call.args[0] for call in probe.call_args_list], ["COM1", "COM2"])


if __name__ == "__main__":
    unittest.main()
