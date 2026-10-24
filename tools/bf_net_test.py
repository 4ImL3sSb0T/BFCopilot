#!/usr/bin/env python3
"""Exercise the BSP TCP/UDP endpoints from a host computer."""

import argparse
import concurrent.futures
import socket
import struct
import threading
import time


def recv_exact(connection, size, timeout):
    connection.settimeout(timeout)
    data = bytearray()
    while len(data) < size:
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
                if address[0] != host or echoed != payload:
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


def udp_reliability_test(host, port, messages, payload_size, timeout):
    passed = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.settimeout(timeout)
        for sequence in range(messages):
            payload = pattern(payload_size, sequence + 2001)
            connection.sendto(payload, (host, port))
            echoed, address = connection.recvfrom(2048)
            if address[0] != host or echoed != payload:
                raise AssertionError(f"UDP reliability mismatch at message {sequence + 1}")
            passed += 1
    return passed


def tcp_stress_worker(host, port, worker, duration, payload_size, timeout):
    deadline = time.monotonic() + duration
    messages = 0
    bytes_sent = 0
    try:
        with socket.create_connection((host, port), timeout=timeout) as connection:
            while time.monotonic() < deadline:
                payload = stress_payload(payload_size, worker, messages + 1)
                connection.sendall(payload)
                if recv_exact(connection, payload_size, timeout) != payload:
                    raise AssertionError(f"TCP stress mismatch in worker {worker}")
                messages += 1
                bytes_sent += payload_size
    except Exception as error:
        return {"worker": worker, "messages": messages, "bytes": bytes_sent, "error": error}
    return {"worker": worker, "messages": messages, "bytes": bytes_sent, "error": None}


def tcp_stress_test(host, port, workers, duration, payload_size, timeout):
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(tcp_stress_worker, host, port, worker, duration,
                                    payload_size, timeout)
                   for worker in range(workers)]
        results = [future.result() for future in futures]
    for result in results:
        if result["error"] is not None:
            raise result["error"]
    return sum(result["messages"] for result in results), sum(result["bytes"] for result in results)


def udp_stress_worker(host, port, worker, duration, payload_size, burst, timeout):
    deadline = time.monotonic() + duration
    sent = 0
    received = 0
    malformed = 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.settimeout(min(timeout, 0.05))
        while time.monotonic() < deadline:
            for _ in range(burst):
                sent += 1
                connection.sendto(stress_payload(payload_size, worker, sent), (host, port))
            while True:
                try:
                    echoed, address = connection.recvfrom(2048)
                except TimeoutError:
                    break
                valid = address[0] == host and len(echoed) == payload_size
                if valid:
                    echoed_worker, echoed_sequence = struct.unpack_from("<II", echoed)
                    valid = echoed == stress_payload(payload_size, echoed_worker, echoed_sequence)
                if not valid:
                    malformed += 1
                else:
                    received += 1
    return sent, received, malformed


def udp_stress_test(host, port, workers, duration, payload_size, burst, timeout):
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(udp_stress_worker, host, port, worker, duration,
                                    payload_size, burst, timeout)
                   for worker in range(workers)]
        results = [future.result() for future in futures]
    sent = sum(result[0] for result in results)
    received = sum(result[1] for result in results)
    malformed = sum(result[2] for result in results)
    return sent, received, malformed


def client_echo_server(bind_host, port, rounds, timeout):
    passed = 0
    deadline = time.monotonic() + max(timeout * 2, rounds * 3.0)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((bind_host, port))
        listener.listen(1)
        listener.settimeout(max(0.1, deadline - time.monotonic()))
        connection, _ = listener.accept()
        with connection:
            while time.monotonic() < deadline and passed < rounds:
                payload = recv_exact(connection, 64, timeout)
                if payload[:4] != b"BNET":
                    raise AssertionError("board TCP client sent an invalid marker")
                sequence = struct.unpack_from("<I", payload, 4)[0]
                if sequence == 0:
                    raise AssertionError("board TCP client sent sequence zero")
                connection.sendall(payload)
                passed += 1
    if passed < rounds:
        raise TimeoutError(f"board TCP client completed {passed}/{rounds} frames")
    return passed


def main():
    parser = argparse.ArgumentParser(description="BSP TCP/UDP network test")
    parser.add_argument("--host", required=True, help="board IPv4 address")
    parser.add_argument("--tcp-port", type=int, default=3333)
    parser.add_argument("--udp-port", type=int, default=3334)
    parser.add_argument("--client-listen-port", type=int,
                        help="listen for the board TCP client; match CONFIG_BF_NET_TEST_REMOTE_PORT")
    parser.add_argument("--client-listen-host", default="0.0.0.0")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--mode", choices=("basic", "stress", "reliability", "all"), default="basic")
    parser.add_argument("--duration", type=float, default=10.0,
                        help="stress test duration in seconds")
    parser.add_argument("--tcp-workers", type=int, default=4)
    parser.add_argument("--udp-workers", type=int, default=2)
    parser.add_argument("--udp-burst", type=int, default=16)
    parser.add_argument("--stress-payload-size", type=int, default=1024)
    parser.add_argument("--reconnects", type=int, default=50)
    parser.add_argument("--reliability-messages", type=int, default=200)
    parser.add_argument("--reliability-payload-size", type=int, default=128)
    args = parser.parse_args()
    if args.rounds < 1 or args.timeout <= 0 or args.duration <= 0:
        parser.error("rounds and duration must be positive; timeout must be greater than zero")
    if args.tcp_workers < 1 or args.udp_workers < 1 or args.udp_burst < 1:
        parser.error("worker and burst counts must be positive")
    if args.reconnects < 1 or args.reliability_messages < 1:
        parser.error("reliability counts must be positive")
    if args.stress_payload_size < 8 or not 1 <= args.reliability_payload_size <= 1024:
        parser.error("payload sizes must be between 1 and 1024; stress payload needs at least 8")
    for name in ("tcp_port", "udp_port", "client_listen_port"):
        port = getattr(args, name)
        if port is not None and not 1 <= port <= 65535:
            parser.error(f"{name.replace('_', '-')} must be between 1 and 65535")

    client_thread = None
    client_result = []
    client_error = []
    if args.client_listen_port is not None:
        def run_client_server():
            try:
                client_result.append(client_echo_server(args.client_listen_host,
                                                        args.client_listen_port,
                                                        args.rounds, args.timeout))
            except BaseException as error:
                client_error.append(error)

        client_thread = threading.Thread(target=run_client_server, daemon=True)
        client_thread.start()
        time.sleep(0.05)

    results = []
    if args.mode in ("basic", "all"):
        tcp_passed = tcp_echo_test(args.host, args.tcp_port, args.rounds, args.timeout)
        udp_passed = udp_echo_test(args.host, args.udp_port, args.rounds, args.timeout)
        results.append(f"tcp={tcp_passed} udp={udp_passed}")
    if args.mode in ("reliability", "all"):
        tcp_reconnects = tcp_reliability_test(args.host, args.tcp_port, args.reconnects,
                                               args.reliability_payload_size, args.timeout)
        udp_messages = udp_reliability_test(args.host, args.udp_port,
                                             args.reliability_messages,
                                             args.reliability_payload_size, args.timeout)
        results.append(f"reliability_tcp={tcp_reconnects} reliability_udp={udp_messages}")
    if args.mode in ("stress", "all"):
        tcp_messages, tcp_bytes = tcp_stress_test(args.host, args.tcp_port, args.tcp_workers,
                                                   args.duration, args.stress_payload_size,
                                                   args.timeout)
        udp_sent, udp_received, udp_malformed = udp_stress_test(
            args.host, args.udp_port, args.udp_workers, args.duration,
            args.stress_payload_size, args.udp_burst, args.timeout)
        loss = 100.0 * (udp_sent - udp_received) / udp_sent if udp_sent else 0.0
        if udp_malformed:
            raise AssertionError(f"UDP stress received {udp_malformed} malformed packets")
        results.append(f"stress_tcp={tcp_messages} tcp_bytes={tcp_bytes} "
                       f"stress_udp_sent={udp_sent} stress_udp_echoed={udp_received} "
                       f"stress_udp_loss={loss:.2f}%")
    if client_thread is not None:
        client_thread.join(max(args.timeout * 2, args.rounds * 3.0) + 1.0)
        if client_thread.is_alive():
            raise TimeoutError("board TCP client test did not finish")
        if client_error:
            raise client_error[0]

    results.append(f"client={client_result[0] if client_result else 0}")
    print("PASS " + " ".join(results))


if __name__ == "__main__":
    main()
