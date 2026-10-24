#!/usr/bin/env python3
"""Exercise the BSP TCP/UDP endpoints from a host computer."""

import argparse
import concurrent.futures
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import select
import socket
import struct
import threading
import time


def recv_exact(connection, size, timeout):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while len(data) < size:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"TCP frame deadline exceeded ({len(data)}/{size} bytes)")
        connection.settimeout(remaining)
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise ConnectionError("board closed the TCP connection")
        data.extend(chunk)
    return bytes(data)


def pattern(size, seed):
    return bytes((seed * 29 + index * 17 + 0x5A) & 0xFF for index in range(size))


def stress_payload(size, worker, sequence):
    if size < 8:
        raise ValueError("stress payload size must be at least 8")
    payload = bytearray(pattern(size, worker * 1000003 + sequence))
    struct.pack_into("<II", payload, 0, worker, sequence)
    return bytes(payload)


def tcp_echo_test(host, port, rounds, timeout):
    sizes = (1, 2, 3, 7, 31, 64, 255, 1024)
    passed = 0
    for reconnect in range(rounds):
        with socket.create_connection((host, port), timeout=timeout) as connection:
            for index, size in enumerate(sizes):
                payload = pattern(size, reconnect * len(sizes) + index + 1)
                cursor = 0
                while cursor < size:
                    step = min(size - cursor, 1 + ((cursor + index) % 17))
                    connection.sendall(payload[cursor:cursor + step])
                    cursor += step
                if recv_exact(connection, size, timeout) != payload:
                    raise AssertionError(f"TCP echo mismatch at size {size}")
                passed += 1
    return passed


def udp_echo_test(host, port, rounds, timeout):
    sizes = (0, 1, 2, 31, 255, 1024)
    passed = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.settimeout(timeout)
        for reconnect in range(rounds):
            for index, size in enumerate(sizes):
                payload = pattern(size, reconnect * len(sizes) + index + 71)
                connection.sendto(payload, (host, port))
                echoed, address = connection.recvfrom(2048)
                if address != (host, port) or echoed != payload:
                    raise AssertionError(f"UDP echo mismatch at size {size}")
                passed += 1
    return passed


def tcp_reliability_test(host, port, reconnects, payload_size, timeout):
    passed = 0
    for reconnect in range(reconnects):
        payload = pattern(payload_size, reconnect + 1001)
        with socket.create_connection((host, port), timeout=timeout) as connection:
            connection.sendall(payload)
            if recv_exact(connection, payload_size, timeout) != payload:
                raise AssertionError(f"TCP reliability mismatch at reconnect {reconnect + 1}")
        passed += 1
    return passed


def udp_reliability_test(host, port, messages, payload_size, timeout, max_loss):
    passed = 0
    lost = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.connect((host, port))
        for sequence in range(messages):
            payload = stress_payload(payload_size, 0, sequence + 1)
            matched = False
            connection.settimeout(timeout)
            connection.send(payload)
            deadline = time.monotonic() + timeout
            while not matched:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                connection.settimeout(remaining)
                try:
                    echoed = connection.recv(65535)
                except TimeoutError:
                    break
                if len(echoed) != payload_size:
                    raise AssertionError(f"UDP reliability size mismatch at {sequence + 1}")
                echoed_worker, echoed_sequence = struct.unpack_from("<II", echoed)
                if echoed_worker != 0 or not 1 <= echoed_sequence <= sequence + 1 or \
                        echoed != stress_payload(payload_size, 0, echoed_sequence):
                    raise AssertionError(f"UDP reliability corruption at {sequence + 1}")
                matched = echoed_sequence == sequence + 1
            if matched:
                passed += 1
            else:
                lost += 1
    if lost > messages * max_loss:
        raise AssertionError(f"UDP reliability lost {lost}/{messages} messages "
                             f"(allowed {max_loss:.1%})")
    return passed, lost


def tcp_stress_worker(host, port, worker, duration, payload_size, timeout):
    started = time.monotonic()
    deadline = started + duration
    messages = 0
    bytes_sent = 0
    latencies = []
    try:
        with socket.create_connection((host, port), timeout=timeout) as connection:
            while time.monotonic() < deadline:
                payload = stress_payload(payload_size, worker, messages + 1)
                sent_at = time.monotonic()
                connection.sendall(payload)
                if recv_exact(connection, payload_size, timeout) != payload:
                    raise AssertionError(f"TCP stress mismatch in worker {worker}")
                messages += 1
                bytes_sent += payload_size
                latencies.append((time.monotonic() - sent_at) * 1000)
    except Exception as error:
        return {"worker": worker, "messages": messages, "bytes": bytes_sent,
                "elapsed_seconds": round(time.monotonic() - started, 3),
                "latency_ms": latency_summary(latencies), "error": f"{type(error).__name__}: {error}"}
    return {"worker": worker, "messages": messages, "bytes": bytes_sent,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "latency_ms": latency_summary(latencies), "error": None}


def tcp_stress_test(host, port, workers, duration, payload_size, timeout):
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(tcp_stress_worker, host, port, worker, duration,
                                    payload_size, timeout)
                   for worker in range(workers)]
        results = [future.result() for future in futures]
    return {"messages": sum(result["messages"] for result in results),
            "bytes": sum(result["bytes"] for result in results), "workers": results}


def latency_summary(samples):
    ordered = sorted(samples)
    if not ordered:
        return {}
    return {name: round(ordered[min(len(ordered) - 1, math.ceil(len(ordered) * ratio) - 1)], 3)
            for name, ratio in (("p50", 0.5), ("p95", 0.95), ("p99", 0.99), ("max", 1.0))}


def udp_stress_worker(host, port, worker, duration, payload_size, burst, timeout, rate):
    started = time.monotonic()
    deadline = started + duration
    sent = 0
    received = 0
    malformed = 0
    send_errors = 0
    duplicates = 0
    reordered = 0
    highest_sequence = 0
    seen = set()
    sent_at = []
    latencies = []
    last_receive = None
    max_receive_gap = 0.0
    next_send = started
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.connect((host, port))
        connection.setblocking(False)
        while time.monotonic() < deadline + timeout:
            now = time.monotonic()
            if now < deadline and now >= next_send:
                for _ in range(burst):
                    try:
                        connection.send(stress_payload(payload_size, worker, sent + 1))
                    except (BlockingIOError, TimeoutError):
                        send_errors += 1
                        break
                    sent_at.append(time.monotonic())
                    sent += 1
                next_send = max(next_send + burst / rate, now) if rate else now
            for _ in range(256):
                try:
                    echoed = connection.recv(65535)
                except BlockingIOError:
                    break
                valid = len(echoed) == payload_size
                if valid:
                    echoed_worker, echoed_sequence = struct.unpack_from("<II", echoed)
                    valid = echoed_worker == worker and 1 <= echoed_sequence <= sent and \
                            echoed == stress_payload(payload_size, worker, echoed_sequence)
                if not valid:
                    malformed += 1
                elif echoed_sequence in seen:
                    duplicates += 1
                else:
                    seen.add(echoed_sequence)
                    received += 1
                    reordered += echoed_sequence < highest_sequence
                    highest_sequence = max(highest_sequence, echoed_sequence)
                    received_at = time.monotonic()
                    if last_receive is not None:
                        max_receive_gap = max(max_receive_gap, received_at - last_receive)
                    last_receive = received_at
                    latencies.append((received_at - sent_at[echoed_sequence - 1]) * 1000)
            now = time.monotonic()
            if now >= deadline and received == sent:
                break
            wait = min(0.01, max(0, (next_send if now < deadline else deadline + timeout) - now))
            select.select([connection], [], [], wait)
    return {"sent": sent, "received": received, "lost": sent - received,
            "malformed": malformed, "send_errors": send_errors, "duplicates": duplicates,
            "reordered": reordered, "max_receive_gap_ms": round(max_receive_gap * 1000, 3),
            "latency_ms": latency_summary(latencies)}


def udp_stress_test(host, port, workers, duration, payload_size, burst, timeout, rate):
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(udp_stress_worker, host, port, worker, duration,
                                    payload_size, burst, timeout, rate)
                   for worker in range(workers)]
        results = [future.result() for future in futures]
    totals = {name: sum(result[name] for result in results)
              for name in ("sent", "received", "lost", "malformed", "send_errors", "duplicates", "reordered")}
    totals["loss_ratio"] = totals["lost"] / totals["sent"] if totals["sent"] else 0.0
    totals["workers"] = results
    return totals


def fault_recovery_test(host, tcp_port, udp_port, rounds, timeout, available_slots):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.connect((host, udp_port))
        connection.settimeout(timeout)
        for size in (1025, 2048, 8192):
            connection.send(pattern(size, size))
            connection.settimeout(min(timeout, 0.2))
            try:
                connection.recv(65535)
            except TimeoutError:
                pass
            else:
                raise AssertionError(f"oversized UDP datagram was echoed: {size}")
        for payload in (b"", stress_payload(1024, 99, 1)):
            connection.settimeout(timeout)
            connection.send(payload)
            if connection.recv(65535) != payload:
                raise AssertionError("UDP failed to recover after oversized datagrams")
    for sequence in range(rounds):
        with socket.create_connection((host, tcp_port), timeout=timeout) as connection:
            connection.sendall(stress_payload(1024, 99, sequence + 1))
            linger = struct.pack("HH" if os.name == "nt" else "ii", 1, 0)
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        time.sleep(0.05)
    tcp_echo_test(host, tcp_port, 1, timeout)
    held = []
    try:
        for sequence in range(available_slots):
            connection = socket.create_connection((host, tcp_port), timeout=timeout)
            held.append(connection)
            payload = stress_payload(64, 100, sequence + 1)
            connection.sendall(payload)
            if recv_exact(connection, len(payload), timeout) != payload:
                raise AssertionError("TCP capacity lost after abrupt disconnects")
        with socket.create_connection((host, tcp_port), timeout=timeout) as excess:
            try:
                excess.sendall(b"excess")
                rejected = excess.recv(64) == b""
            except ConnectionResetError:
                rejected = True
            if not rejected:
                raise AssertionError("connection above configured capacity was not rejected")
        udp_echo_test(host, udp_port, 1, timeout)
    finally:
        for connection in held:
            connection.close()
    time.sleep(0.1)
    with socket.create_connection((host, tcp_port), timeout=timeout) as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow.settimeout(0.1)
        flood_deadline = time.monotonic() + 1.0
        while time.monotonic() < flood_deadline:
            try:
                slow.sendall(pattern(1024, 77))
            except (TimeoutError, ConnectionError):
                break
        if available_slots > 1:
            tcp_echo_test(host, tcp_port, 1, timeout)
        udp_echo_test(host, udp_port, 1, timeout)
    time.sleep(0.2)
    tcp_echo_test(host, tcp_port, 1, timeout)
    return {"tcp_aborts": rounds, "tcp_capacity": available_slots,
            "oversized_udp_dropped": 3, "slow_reader_recovered": True}


def client_echo_server(bind_host, port, rounds, timeout, stop=None, ready=None, reconnect_every=0,
                       idle_timeout=10.0, progress=None):
    passed = 0
    deadline = time.monotonic() + max(timeout * 2, rounds * 3.0)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((bind_host, port))
        listener.listen(1)
        listener.settimeout(0.1)
        if ready is not None:
            ready.set()
        last_sequence = 0
        last_progress = time.monotonic()
        while (stop is not None and not stop.is_set()) or \
                (passed < rounds and time.monotonic() < deadline):
            if time.monotonic() - last_progress >= idle_timeout:
                raise TimeoutError(f"board TCP client stalled after {passed} frames")
            try:
                connection, _ = listener.accept()
            except TimeoutError:
                continue
            if progress is not None:
                progress["connections"] += 1
            with connection:
                while stop is None or not stop.is_set() or passed < rounds:
                    if time.monotonic() - last_progress >= idle_timeout:
                        raise TimeoutError(f"board TCP client stalled after {passed} frames")
                    readable, _, _ = select.select([connection], [], [], 0.1)
                    if not readable:
                        if (stop is None or stop.is_set()) and time.monotonic() >= deadline:
                            break
                        continue
                    try:
                        payload = recv_exact(connection, 64, timeout)
                    except ConnectionError:
                        break
                    sequence = struct.unpack_from("<I", payload, 4)[0]
                    expected = b"BNET" + struct.pack("<I", sequence) + bytes(
                        0x40 + ((sequence * 17 + index * 13) & 0x3F) for index in range(8, 64))
                    if sequence == 0 or sequence <= last_sequence or payload != expected:
                        raise AssertionError("board TCP client payload/sequence mismatch")
                    connection.sendall(payload)
                    last_sequence = sequence
                    last_progress = time.monotonic()
                    passed += 1
                    if progress is not None:
                        progress["frames"] = passed
                    if stop is None and passed >= rounds:
                        break
                    if reconnect_every and passed % reconnect_every == 0:
                        break
    if passed < rounds:
        raise TimeoutError(f"board TCP client completed {passed}/{rounds} frames")
    return passed


class SerialMonitor:
    def __init__(self, port, log_path):
        import serial
        self.connection = serial.Serial(port, 115200, timeout=0.2)
        self.log = Path(log_path).open("w", encoding="utf-8")
        self.stop = threading.Event()
        self.samples = []
        self.faults = []
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        try:
            while not self.stop.is_set():
                line = self.connection.readline().decode("utf-8", "replace").strip()
                if not line:
                    continue
                self.log.write(line + "\n")
                self.log.flush()
                if re.search(r"Guru Meditation|panic'ed|assert failed|Brownout|ESP-ROM:|Stack smashing|watchdog got triggered|CORRUPT HEAP|HEAP CORRUPTION", line):
                    self.faults.append(line)
                if "health:" in line:
                    self.samples.append({key: int(value) for key, value in
                                         re.findall(r"(\w+)=(\d+)", line)})
        except Exception as error:
            self.error = str(error)

    def close(self):
        self.stop.set()
        self.thread.join(2)
        self.connection.close()
        self.log.close()
        summary = {}
        for key in ("heap_free", "heap_min", "heap_largest", "stack_free", "net_stack_free"):
            values = [sample[key] for sample in self.samples if key in sample]
            if values:
                summary[key] = {"first": values[0], "last": values[-1], "min": min(values), "max": max(values)}
        return {"samples": self.samples, "summary": summary, "faults": self.faults, "error": self.error}


def main():
    parser = argparse.ArgumentParser(description="BSP TCP/UDP network test")
    parser.add_argument("--host", required=True, help="board IPv4 address")
    parser.add_argument("--tcp-port", type=int, default=3333)
    parser.add_argument("--udp-port", type=int, default=3334)
    parser.add_argument("--client-listen-port", type=int,
                        help="listen for the board TCP client; match CONFIG_BF_NET_TEST_REMOTE_PORT")
    parser.add_argument("--client-listen-host", default="0.0.0.0")
    parser.add_argument("--client-reconnect-every", type=int, default=0,
                        help="close board client connection after N frames to exercise reconnects")
    parser.add_argument("--client-idle-timeout", type=float, default=10.0,
                        help="fail if board client stops producing frames for this many seconds")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--mode", choices=("basic", "stress", "reliability", "fault", "all"), default="basic")
    parser.add_argument("--duration", type=float, default=10.0,
                        help="stress test duration in seconds")
    parser.add_argument("--tcp-workers", type=int, default=3)
    parser.add_argument("--udp-workers", type=int, default=2)
    parser.add_argument("--udp-burst", type=int, default=16)
    parser.add_argument("--udp-rate", type=float, default=0.0,
                        help="per-worker UDP packets per second; 0 sends as fast as possible")
    parser.add_argument("--stress-payload-size", type=int, default=1024)
    parser.add_argument("--reconnects", type=int, default=50)
    parser.add_argument("--reliability-messages", type=int, default=200)
    parser.add_argument("--reliability-payload-size", type=int, default=128)
    parser.add_argument("--max-loss", type=float, default=0.01,
                        help="allowed UDP reliability loss ratio (default 1%%)")
    parser.add_argument("--max-tcp-connections", type=int, default=4,
                        help="board BSP_NET_MAX_TCP_CONNECTIONS; an enabled board TCP "
                             "client reserves one slot")
    parser.add_argument("--board-client-enabled", action="store_true",
                        help="reserve one TCP slot even without a host echo listener")
    parser.add_argument("--stress-max-loss", type=float,
                        help="fail stress above this UDP loss ratio; omitted measures overload only")
    parser.add_argument("--fault-rounds", type=int, default=30)
    parser.add_argument("--report-json", type=Path)
    parser.add_argument("--serial-port", help="monitor board console and detect resets/panics")
    parser.add_argument("--serial-log", type=Path, default=Path("net-test.serial.log"))
    args = parser.parse_args()
    if not all(math.isfinite(value) for value in (args.timeout, args.duration, args.udp_rate,
                                                  args.client_idle_timeout)):
        parser.error("timeout, duration and udp-rate must be finite")
    if args.rounds < 1 or args.timeout <= 0 or args.duration <= 0:
        parser.error("rounds and duration must be positive; timeout must be greater than zero")
    if args.tcp_workers < 1 or args.udp_workers < 1 or args.udp_burst < 1:
        parser.error("worker and burst counts must be positive")
    if args.udp_rate < 0:
        parser.error("udp-rate must not be negative")
    if args.reconnects < 1 or args.reliability_messages < 1:
        parser.error("reliability counts must be positive")
    if not 8 <= args.stress_payload_size <= 1024 or not 8 <= args.reliability_payload_size <= 1024:
        parser.error("stress and reliability payload sizes must be between 8 and 1024")
    if not 0 <= args.max_loss < 1:
        parser.error("max-loss must be in [0, 1)")
    if args.max_tcp_connections < 1:
        parser.error("max-tcp-connections must be positive")
    if args.stress_max_loss is not None and not 0 <= args.stress_max_loss <= 1:
        parser.error("stress-max-loss must be in [0, 1]")
    if args.fault_rounds < 1:
        parser.error("fault-rounds must be positive")
    if args.client_reconnect_every < 0:
        parser.error("client-reconnect-every must not be negative")
    if args.client_idle_timeout <= 0:
        parser.error("client-idle-timeout must be positive")
    reserved = int(args.board_client_enabled or args.client_listen_port is not None)
    available_slots = args.max_tcp_connections - reserved
    if available_slots < 1:
        parser.error("board must have at least one TCP server slot")
    if args.mode in ("stress", "all") and \
            args.tcp_workers > available_slots:
        parser.error("tcp workers exceed board TCP slots (BSP_NET_MAX_TCP_CONNECTIONS); "
                     "reduce --tcp-workers or raise --max-tcp-connections")
    for name in ("tcp_port", "udp_port", "client_listen_port"):
        port = getattr(args, name)
        if port is not None and not 1 <= port <= 65535:
            parser.error(f"{name.replace('_', '-')} must be between 1 and 65535")
    try:
        socket.inet_pton(socket.AF_INET, args.host)
    except OSError:
        parser.error("host must be an IPv4 address")
    if args.report_json is not None:
        args.report_json.parent.mkdir(parents=True, exist_ok=True)
    args.serial_log.parent.mkdir(parents=True, exist_ok=True)
    report = {"status": "RUNNING", "started_at": datetime.now(timezone.utc).isoformat(),
              "config": {key: str(value) if isinstance(value, Path) else value
                                                for key, value in vars(args).items()}, "stages": {}}
    started = time.monotonic()
    client_thread = None
    client_result = []
    client_error = []
    client_progress = {"frames": 0, "connections": 0}
    client_stop = threading.Event()
    client_ready = threading.Event()
    monitor = None

    def save_report():
        if args.report_json is not None:
            args.report_json.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8")

    def stage(name, action):
        print(f"RUN {name}", flush=True)
        stage_started = time.monotonic()
        try:
            result = action()
        except Exception as error:
            report["stages"][name] = {"elapsed_seconds": round(time.monotonic() - stage_started, 3),
                                      "error": f"{type(error).__name__}: {error}"}
            save_report()
            raise
        report["stages"][name] = {"elapsed_seconds": round(time.monotonic() - stage_started, 3),
                                  "result": result}
        save_report()
        print(f"DONE {name} {json.dumps(result)}", flush=True)
        return result

    try:
        save_report()
        if args.serial_port:
            monitor = SerialMonitor(args.serial_port, args.serial_log)
            monitor.thread.start()
        if args.client_listen_port is not None:
            def run_client_server():
                try:
                    client_result.append(client_echo_server(args.client_listen_host,
                                                            args.client_listen_port,
                                                            args.rounds, args.timeout,
                                                            client_stop, client_ready,
                                                            args.client_reconnect_every,
                                                            args.client_idle_timeout,
                                                            client_progress))
                except Exception as error:
                    client_error.append(str(error))
                    client_ready.set()
            client_thread = threading.Thread(target=run_client_server, daemon=True)
            client_thread.start()
            if not client_ready.wait(args.timeout) or client_error:
                raise RuntimeError(f"client listener failed: {client_error}")
        if args.mode in ("basic", "all"):
            stage("basic_tcp", lambda: tcp_echo_test(args.host, args.tcp_port, args.rounds, args.timeout))
            stage("basic_udp", lambda: udp_echo_test(args.host, args.udp_port, args.rounds, args.timeout))
        if args.mode in ("reliability", "all"):
            stage("tcp_reconnect", lambda: tcp_reliability_test(
                args.host, args.tcp_port, args.reconnects, args.reliability_payload_size, args.timeout))
            stage("udp_reliability", lambda: udp_reliability_test(
                args.host, args.udp_port, args.reliability_messages,
                args.reliability_payload_size, args.timeout, args.max_loss))
        if args.mode in ("fault", "all"):
            stage("fault_recovery", lambda: fault_recovery_test(
                args.host, args.tcp_port, args.udp_port, args.fault_rounds, args.timeout, available_slots))
        if args.mode in ("stress", "all"):
            def mixed_stress():
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                    tcp_future = executor.submit(tcp_stress_test, args.host, args.tcp_port,
                        args.tcp_workers, args.duration, args.stress_payload_size, args.timeout)
                    udp_future = executor.submit(udp_stress_test, args.host, args.udp_port,
                        args.udp_workers, args.duration, args.stress_payload_size,
                        args.udp_burst, args.timeout, args.udp_rate)
                    return {"tcp": tcp_future.result(), "udp": udp_future.result()}
            result = stage("mixed_stress", mixed_stress)
            issues = []
            for name, action in (
                ("post_stress_tcp", lambda: tcp_echo_test(args.host, args.tcp_port, 1, args.timeout)),
                ("post_stress_udp", lambda: udp_echo_test(args.host, args.udp_port, 1, args.timeout)),
            ):
                try:
                    stage(name, action)
                except Exception as error:
                    issues.append(f"{name}: {type(error).__name__}: {error}")
            if any(worker["error"] is not None for worker in result["tcp"]["workers"]):
                issues.append("TCP stress worker failed")
            udp = result["udp"]
            if not udp["sent"] or udp["malformed"] or udp["duplicates"]:
                issues.append("UDP stress empty/corrupt/duplicate response")
            if args.stress_max_loss is not None and (udp["loss_ratio"] > args.stress_max_loss or udp["send_errors"]):
                issues.append("UDP stress loss/send errors exceed acceptance limit")
            if issues:
                raise AssertionError("; ".join(issues))
        if client_thread is not None:
            client_stop.set()
            client_thread.join(max(args.timeout * 2, args.rounds * 3) + 1)
            if client_thread.is_alive() or client_error or not client_result or client_result[0] < args.rounds:
                raise AssertionError(f"board client failed: counts={client_result}, errors={client_error}")
        report["status"] = "PASS"
    except (Exception, KeyboardInterrupt) as error:
        report["status"] = "FAIL"
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        client_stop.set()
        if client_thread is not None:
            client_thread.join(args.timeout + 2)
        report["client_frames"] = client_progress["frames"]
        report["client_connections"] = client_progress["connections"]
        report["client_errors"] = client_error
        if client_error:
            report["status"] = "FAIL"
        if monitor is not None:
            report["serial"] = monitor.close()
            if report["serial"]["faults"] or report["serial"]["error"]:
                report["status"] = "FAIL"
                report["serial_error"] = "board reset/panic or serial monitor failure"
            if any(sample.get("client_mismatch", 0) for sample in report["serial"]["samples"]):
                report["status"] = "FAIL"
                report["serial_error"] = "board client reported corrupt echo data"
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        save_report()
    print(f"{report['status']} elapsed={report['elapsed_seconds']}s client={report['client_frames']} "
          f"{report.get('error', '')}", flush=True)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
