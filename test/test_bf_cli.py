import sys
import contextlib
import io
import json
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import bf_cli


class CleanReplyTest(unittest.TestCase):
    def test_drops_log_lines_prompt_and_echo(self):
        raw = (b"bf> param get sensors.vbat_div_num\r\n"
               b"sensors.vbat_div_num = 27.000 \r\n"
               b"bf> "
               b"I (686973) telemetry: state=1 armed=0 vbat=8105mV(1)\r\n")
        self.assertEqual(bf_cli.clean(raw, "param get sensors.vbat_div_num"),
                         "sensors.vbat_div_num = 27.000")

    def test_strips_ansi_and_nul(self):
        raw = b"bf>  \x1b[0Kbf>  status\x1b[34C\r\n\x00power    : vbat=8105 mV\r\n"
        self.assertEqual(bf_cli.clean(raw, "status"), "power    : vbat=8105 mV")

    def test_keeps_multiline_output(self):
        raw = (b"bf> params\r\n"
               b"servo_l.zero_us  1520.000 us\r\n"
               b"servo_r.zero_us  1520.000 us\r\n"
               b"bf> ")
        self.assertEqual(bf_cli.clean(raw, "params").splitlines(),
                         ["servo_l.zero_us  1520.000 us", "servo_r.zero_us  1520.000 us"])

    def test_echo_with_one_lost_leading_character_is_not_swallowed(self):
        raw = b"bf> aram get sensors.vbat_div_num\r\nUnrecognized command\r\nbf> "
        output = bf_cli.clean(raw, "param get sensors.vbat_div_num")
        self.assertIn("Unrecognized command", output)

    def test_blank_reply(self):
        self.assertEqual(bf_cli.clean(b"bf> bf> ", "status"), "")

    def test_log_with_a_lost_level_character_is_filtered(self):
        self.assertEqual(bf_cli.clean(b"(123) telemetry: active\r\n", "status"), "")


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now


class FakePort:
    def __init__(self, clock, events):
        self.clock = clock
        self.events = list(events)
        self.writes = []
        self.in_waiting = 0

    def read(self, size):
        self.clock.now += 0.05
        if self.events and self.events[0][0] <= self.clock.now:
            return self.events.pop(0)[1]
        return b""

    def reset_input_buffer(self):
        pass

    def write(self, payload):
        self.writes.append(payload)
        return len(payload)


class SerialReplyTest(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.timer = patch.object(bf_cli.time, "monotonic", self.clock.monotonic)
        self.timer.start()
        self.addCleanup(self.timer.stop)

    def console(self, events):
        console = bf_cli.Console.__new__(bf_cli.Console)
        console.device = "COM4"
        console._desynchronized = False
        console._pending = b""
        console.port = FakePort(self.clock, events)
        return console

    def test_logs_do_not_extend_reply_silence(self):
        events = [(0, b" status\r\npower : vbat=4052 mV\r\n")]
        events.extend((index / 10, b"I (123) telemetry: active\r\n")
                      for index in range(1, 20))
        console = self.console(events)
        self.assertEqual(console.run("status", 2), "power : vbat=4052 mV")
        self.assertLess(self.clock.now, 0.7)
        self.assertEqual(console.port.writes, [b" status\r"])

    def test_waits_for_delayed_reply_after_echo_and_logs(self):
        console = self.console([(0, b"bf>  status\r\n"),
                                (0.1, b"I (123) telemetry: active\r\n"),
                                (0.9, b"power : vbat=4052 mV\r\n")])
        self.assertEqual(console.run("status", 2), "power : vbat=4052 mV")
        self.assertGreater(self.clock.now, 1.3)

    def test_echo_or_logs_alone_time_out_without_retry(self):
        for raw in (b"bf>  status\r\n", b"I (123) telemetry: active\r\n", b""):
            with self.subTest(raw=raw):
                console = self.console([(0, raw)])
                with self.assertRaises(bf_cli.ReplyTimeout):
                    console.run("status", 0.7)
                with self.assertRaises(bf_cli.ConsoleError):
                    console.run("status", 0.7)
                self.assertEqual(len(console.port.writes), 1)

    def test_unknown_command_is_not_replayed(self):
        console = self.console([(0, b"typo\r\nUnrecognized command\r\nbf> ")])
        self.assertIn("Unrecognized command", console.run("typo", 2))
        self.assertEqual(len(console.port.writes), 1)

    def test_split_response_lines_are_preserved(self):
        console = self.console([(0, b"status\r\npower : vb"),
                                (0.2, b"at=4052 mV\r\nsafety : READY"),
                                (0.4, b"_LOCKED\r\nbf> ")])
        self.assertEqual(console.run("status", 2),
                         "power : vbat=4052 mV\nsafety : READY_LOCKED")

    def test_log_split_across_requests_is_filtered(self):
        console = self.console([(0, b"elemetry: active\r\nstatus\r\npower : vbat=4052 mV\r\n")])
        console._pending = b"I (123) t"
        self.assertEqual(console.run("status", 2), "power : vbat=4052 mV")

    def test_reply_does_not_include_a_trailing_partial_log(self):
        console = self.console([(0, b"status\r\npower : vbat=4052 mV\r\n"),
                                (0.3, b"I (123) telemetry: act")])
        self.assertEqual(console.run("status", 2), "power : vbat=4052 mV")
        self.assertEqual(console._pending, b"I (123) telemetry: act")

    def test_partial_reply_line_must_finish_before_success(self):
        console = self.console([(0, b"status\r\npower : vbat=4052 mV\r\n"),
                                (0.3, b"safety : READY"),
                                (0.7, b"_LOCKED\r\n")])
        self.assertEqual(console.run("status", 2),
                         "power : vbat=4052 mV\nsafety : READY_LOCKED")

    def test_rejects_command_injection_and_overflow_before_writing(self):
        console = self.console([])
        for command in ("status\nservo enable", "status\r", "\x03status", "\x1b[A", "x" * 159):
            with self.subTest(command=command):
                with self.assertRaises(ValueError):
                    console.run(command)
        self.assertEqual(console.port.writes, [])


class JsonOutputTest(unittest.TestCase):
    def invoke(self, arguments, console=None, error=None):
        stdout = io.StringIO()
        with patch.object(bf_cli, "open_console", return_value=console, side_effect=error), \
                contextlib.redirect_stdout(stdout):
            status = bf_cli.main(["--json", *arguments])
        return status, json.loads(stdout.getvalue())

    def test_port_error_is_json(self):
        status, payload = self.invoke(["status"], error=bf_cli.ConsoleError("busy"))
        self.assertEqual(status, 2)
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["error"], "busy")

    def test_json_requires_a_command(self):
        status, payload = self.invoke([])
        self.assertEqual(status, 2)
        self.assertFalse(payload["ok"])

    def test_rejects_invalid_timeout(self):
        for timeout in ("0", "-1", "nan", "inf"):
            with self.subTest(timeout=timeout):
                status, payload = self.invoke(["--timeout", timeout, "status"])
                self.assertEqual(status, 2)
                self.assertFalse(payload["ok"])

    def test_batch_timeout_preserves_results_and_stops(self):
        console = Mock()
        console.run.side_effect = ["sensors.vbat_div_num = 27.000",
                                   bf_cli.ReplyTimeout("timeout", "partial")]
        status, payload = self.invoke(["param get sensors.vbat_div_num",
                                       "param get sensors.vbat_div_den", "status"], console)
        self.assertEqual(status, 2)
        self.assertEqual(len(payload), 2)
        self.assertTrue(payload[0]["ok"])
        self.assertFalse(payload[1]["ok"])
        self.assertEqual(payload[1]["output"], "partial")
        self.assertEqual(console.run.call_count, 2)
        console.close.assert_called_once()

    def test_firmware_errors_are_failures(self):
        for output in ("Command returned non-zero error code: 0x1 (ESP_FAIL)",
                       "set failed: ESP_ERR_INVALID_ARG", "usage: param get <name>"):
            with self.subTest(output=output):
                console = Mock()
                console.run.return_value = output
                status, payload = self.invoke(["param set sensors.vbat_div_den bad"], console)
                self.assertEqual(status, 1)
                self.assertFalse(payload["ok"])


if __name__ == "__main__":
    unittest.main()
