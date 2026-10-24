import concurrent.futures
from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("bf_net_test", ROOT / "tools/bf_net_test.py")
net_test = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(net_test)


class DatagramServer:
    def __init__(self, reply):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(0.02)
        self.port = self.socket.getsockname()[1]
        self.reply = reply
        self.packets = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run)

    def run(self):
        while not self.stop.is_set():
            try:
                payload, address = self.socket.recvfrom(65535)
            except TimeoutError:
                continue
            self.packets.append(payload)
            for response in self.reply(payload, len(self.packets)):
                self.socket.sendto(response, address)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *arguments):
        self.stop.set()
        self.thread.join(1)
        self.socket.close()


class NetworkTestRegression(unittest.TestCase):
    def test_frame_timeout_is_total_deadline(self):
        class SlowSocket:
            def settimeout(self, timeout):
                self.timeout = timeout

            def recv(self, size):
                time.sleep(min(0.04, self.timeout))
                if self.timeout < 0.04:
                    raise TimeoutError("simulated socket deadline")
                return b"x"

        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            net_test.recv_exact(SlowSocket(), 10, 0.1)
        self.assertLess(time.monotonic() - started, 0.25)

    def test_udp_loss_is_measured_without_retries(self):
        with DatagramServer(lambda payload, count: [] if count == 1 else [payload]) as server:
            result = net_test.udp_reliability_test("127.0.0.1", server.port, 4, 64, 0.04, 0.5)
            self.assertEqual(result, (3, 1))
            self.assertEqual(len(server.packets), 4)

    def test_udp_corruption_of_correct_length_fails(self):
        with DatagramServer(lambda payload, count: [payload[:-1] + bytes([payload[-1] ^ 1])]) as server:
            with self.assertRaisesRegex(AssertionError, "corruption"):
                net_test.udp_reliability_test("127.0.0.1", server.port, 1, 64, 0.1, 0)

    def test_sequence_does_not_repeat_after_256_packets(self):
        self.assertNotEqual(net_test.stress_payload(64, 0, 1),
                            net_test.stress_payload(64, 0, 257))

    def test_udp_duplicates_do_not_inflate_delivery(self):
        with DatagramServer(lambda payload, count: [payload, payload]) as server:
            result = net_test.udp_stress_worker("127.0.0.1", server.port, 3, 0.15, 64, 1, 0.05, 50)
            self.assertGreater(result["duplicates"], 0)
            self.assertEqual(result["received"], result["sent"])
            self.assertEqual(result["malformed"], 0)

    def test_udp_wrong_worker_is_rejected(self):
        with DatagramServer(lambda payload, count: [net_test.stress_payload(64, 9, count)]) as server:
            result = net_test.udp_stress_worker("127.0.0.1", server.port, 3, 0.1, 64, 1, 0.05, 30)
            self.assertGreater(result["malformed"], 0)
            self.assertEqual(result["received"], 0)
            self.assertEqual(result["lost"], result["sent"])

    def test_client_server_checks_entire_payload(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        ready = threading.Event()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(net_test.client_echo_server, "127.0.0.1", port,
                                     1, 0.2, None, ready)
            self.assertTrue(ready.wait(1))
            with socket.create_connection(("127.0.0.1", port), timeout=1) as connection:
                connection.sendall(b"BNET\x01\x00\x00\x00" + b"x" * 56)
            with self.assertRaisesRegex(AssertionError, "payload/sequence"):
                future.result(timeout=1)

    def test_client_stall_fails_while_suite_is_running(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        ready = threading.Event()
        stop = threading.Event()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(net_test.client_echo_server, "127.0.0.1", port,
                                     1, 0.2, stop, ready, 0, 0.15)
            self.assertTrue(ready.wait(1))
            with self.assertRaisesRegex(TimeoutError, "stalled"):
                future.result(timeout=1)

    def test_client_validation_continues_across_reconnects(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        ready = threading.Event()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(net_test.client_echo_server, "127.0.0.1", port,
                                     3, 0.3, None, ready, 1)
            self.assertTrue(ready.wait(1))
            for sequence in range(1, 4):
                payload = b"BNET" + struct.pack("<I", sequence) + bytes(
                    0x40 + ((sequence * 17 + index * 13) & 0x3F) for index in range(8, 64))
                with socket.create_connection(("127.0.0.1", port), timeout=1) as connection:
                    connection.sendall(payload)
                    self.assertEqual(net_test.recv_exact(connection, 64, 0.3), payload)
            self.assertEqual(future.result(timeout=1), 3)

    def test_udp_loss_above_threshold_fails(self):
        with DatagramServer(lambda payload, count: []) as server:
            with self.assertRaisesRegex(AssertionError, "lost 2/2"):
                net_test.udp_reliability_test("127.0.0.1", server.port, 2, 64, 0.03, 0)
            self.assertEqual(len(server.packets), 2)

    def test_cli_writes_failure_report(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "failure.json"
            result = subprocess.run([sys.executable, str(ROOT / "tools/bf_net_test.py"),
                "--host", "127.0.0.1", "--tcp-port", str(port), "--timeout", "0.1",
                "--report-json", str(report_path)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 1)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("error", report)

    def test_udp_recovery_is_checked_after_tcp_recovery_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            report_path = Path(directory) / "recovery.json"
            arguments = ["bf_net_test.py", "--host", "127.0.0.1", "--mode", "stress",
                         "--duration", "0.01", "--report-json", str(report_path)]
            with patch.object(sys, "argv", arguments), \
                    patch.object(net_test, "tcp_stress_test", return_value={
                        "workers": [{"error": "TimeoutError"}]}), \
                    patch.object(net_test, "udp_stress_test", return_value={
                        "sent": 1, "malformed": 0, "duplicates": 0}), \
                    patch.object(net_test, "tcp_echo_test", side_effect=TimeoutError("recovery timeout")), \
                    patch.object(net_test, "udp_echo_test", return_value=6) as udp_recovery, \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(net_test.main(), 1)
                udp_recovery.assert_called_once()
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertIn("TimeoutError", report["stages"]["post_stress_tcp"]["error"])
            self.assertEqual(report["stages"]["post_stress_udp"]["result"], 6)

    def test_cli_rejects_invalid_duration_and_payload(self):
        for arguments in (("--duration", "nan"), ("--stress-payload-size", "1025")):
            result = subprocess.run([sys.executable, str(ROOT / "tools/bf_net_test.py"),
                "--host", "127.0.0.1", *arguments], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
