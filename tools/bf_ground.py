import argparse
import json
import math
import queue
import secrets
import struct
import threading
import time
import zlib
from dataclasses import dataclass

HELLO, CONTROL, ARM, DISARM, ESTOP, CLEAR, STATUS, ACK = range(1, 9)
HEADER = struct.Struct("<HBBIIIIHH")
CONTROL_PAYLOAD = struct.Struct("<BBHHHIIHHhhhhhh")
STATUS_PAYLOAD = struct.Struct("<IHBBIiihhHHI")


def cobs_encode(data):
    output = bytearray([0])
    marker = 0
    code = 1
    for value in data:
        if value == 0:
            output[marker] = code
            marker = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 255:
                output[marker] = code
                marker = len(output)
                output.append(0)
                code = 1
    output[marker] = code
    return bytes(output)


def cobs_decode(data):
    output = bytearray()
    position = 0
    while position < len(data):
        code = data[position]
        position += 1
        if not code or position + code - 1 > len(data):
            raise ValueError("invalid COBS")
        chunk = data[position:position + code - 1]
        if 0 in chunk:
            raise ValueError("embedded delimiter")
        output.extend(chunk)
        position += code - 1
        if code != 255 and position < len(data):
            output.append(0)
    return bytes(output)


@dataclass
class Frame:
    kind: int
    session: int = 0
    sequence: int = 0
    request: int = 0
    payload: bytes = bytes(32)

    def encode(self):
        if len(self.payload) != 32 or self.kind not in range(1, 9):
            raise ValueError("invalid frame")
        raw = HEADER.pack(0x4642, 1, self.kind, 1, self.session, self.sequence,
                          self.request, 32, 0) + self.payload
        return raw + struct.pack("<I", zlib.crc32(raw))

    @classmethod
    def decode(cls, raw):
        if len(raw) != 60 or zlib.crc32(raw[:56]) != struct.unpack_from("<I", raw, 56)[0]:
            raise ValueError("length or CRC")
        magic, version, kind, vehicle, session, sequence, request, length, reserved = HEADER.unpack_from(raw)
        if (magic, version, vehicle, length, reserved) != (0x4642, 1, 1, 32, 0) or kind not in range(1, 9):
            raise ValueError("header")
        return cls(kind, session, sequence, request, raw[24:56])


def control_payload(lease, mode=0, frequency=0, left=0, right=0, center_left=0,
                    center_right=0, phase=0, differential=0):
    values = (frequency, left, right, center_left, center_right, phase, differential)
    if not all(math.isfinite(value) for value in values):
        raise ValueError("nonfinite control")
    return CONTROL_PAYLOAD.pack(mode, 0, 100, 0, 0, lease, round(frequency * 1000),
                                round(left * 100), round(right * 100),
                                round(center_left * 100), round(center_right * 100),
                                round(phase * 100), 0, 0, round(differential * 1000))


def read_input(inbox):
    while True:
        try:
            line = input()
        except EOFError:
            line = "quit"
        try:
            inbox.put_nowait((time.monotonic(), line))
        except queue.Full:
            continue
        if line.strip() == "quit":
            return


def run(args):
    import serial

    inbox = queue.Queue(maxsize=8)
    threading.Thread(target=read_input, args=(inbox,), daemon=True).start()
    print("Commands: connect | clear | arm | hold L R | manual L R seconds | "
          "flap Hz A_L A_R C_L C_R phase differential seconds | stop | estop | quit")
    session = sequence = lease = status_sequence = 0
    nonce = 0
    last_status = next_control = last_print = 0.0
    pending_hello = 0.0
    motion_until = 0.0
    desired = {}
    pending = {}
    encoded = bytearray()
    started = 0.0
    overflow = False
    log = open(args.log, "a", encoding="utf-8") if args.log else None
    with serial.Serial(args.port, 115200, timeout=0.005, write_timeout=0.05) as port:
        port.reset_input_buffer()

        def send(kind, payload=bytes(32), request=0, target_session=None):
            nonlocal sequence
            sequence += 1
            frame = Frame(kind, session if target_session is None else target_session,
                          sequence, request, payload)
            port.write(cobs_encode(frame.encode()) + b"\0")

        def request(kind):
            identifier = secrets.randbits(32) or 1
            send(kind, request=identifier)
            pending[identifier] = (kind, time.monotonic(), 0)

        try:
            while True:
                now = time.monotonic()
                if encoded and now - started >= 0.1:
                    encoded.clear()
                    overflow = True
                for value in port.read(128):
                    if value:
                        if not encoded:
                            started = now
                        if len(encoded) >= 61:
                            overflow = True
                        if not overflow:
                            encoded.append(value)
                        continue
                    try:
                        if overflow:
                            raise ValueError("overflow")
                        frame = Frame.decode(cobs_decode(encoded))
                        if frame.kind == STATUS and frame.request == nonce and nonce:
                            if not session and now < pending_hello:
                                session = frame.session
                                status_sequence = 0
                                desired = {}
                                pending.clear()
                                print("Session established, locked; calibrate before clear/arm.")
                            if not session or frame.session != session or frame.sequence <= status_sequence:
                                continue
                            status_sequence = frame.sequence
                            status = STATUS_PAYLOAD.unpack(frame.payload)
                            lease = status[0]
                            last_status = now
                            record = dict(zip(("lease", "lease_ms", "state", "armed", "faults", "vbat_mv",
                                               "iservo_ma", "left_command_cd", "right_command_cd", "phase_cd",
                                               "rx_age_ms", "rejected"), status))
                            if log:
                                log.write(json.dumps({"time": time.time(), **record}) + "\n")
                                log.flush()
                            if now - last_print >= 1:
                                print(json.dumps(record))
                                last_print = now
                        elif frame.kind == ACK and session and frame.session == session:
                            if frame.request in pending and pending[frame.request][0] == frame.payload[4]:
                                print("ACK", frame.payload[4], "ok" if struct.unpack_from("<I", frame.payload)[0] == 0 else "refused")
                                pending.pop(frame.request)
                    except ValueError:
                        pass
                    finally:
                        encoded.clear()
                        overflow = False
                if session and now - last_status >= 0.15:
                    session = lease = 0
                    desired = {}
                    pending.clear()
                    print("Telemetry expired; session closed. Explicit connect required.")
                if motion_until and now >= motion_until:
                    if session:
                        request(DISARM)
                    motion_until = 0
                    desired = {}
                if session and lease and now >= next_control:
                    send(CONTROL, control_payload(lease, **desired))
                    next_control = now + 0.02
                for identifier, (kind, sent, attempts) in list(pending.items()):
                    if now - sent >= 0.08:
                        if attempts >= 2:
                            pending.pop(identifier)
                            print("ACK timeout", kind)
                        else:
                            send(kind, request=identifier)
                            pending[identifier] = (kind, now, attempts + 1)
                try:
                    entered, line = inbox.get_nowait()
                except queue.Empty:
                    continue
                words = line.split()
                if not words:
                    continue
                action = words[0]
                if action in ("quit", "estop"):
                    send(ESTOP, target_session=0)
                    session = lease = 0
                    pending.clear()
                    if action == "quit":
                        break
                    continue
                if action == "connect":
                    session = lease = status_sequence = 0
                    pending.clear()
                    nonce = secrets.randbits(32) or 1
                    pending_hello = now + 0.5
                    desired = {}
                    send(HELLO, request=nonce, target_session=0)
                    continue
                if not session or now - entered >= 0.15:
                    print("No fresh session/input; command refused")
                    continue
                try:
                    if action in ("arm", "clear", "stop"):
                        if action == "arm" and desired.get("mode", 0) == 2:
                            raise ValueError("select hold before arm")
                        if action == "stop":
                            desired = {}
                            motion_until = 0
                        request({"arm": ARM, "clear": CLEAR, "stop": DISARM}[action])
                    elif action in ("hold", "manual"):
                        expected = 3 if action == "hold" else 4
                        if len(words) != expected:
                            raise ValueError("wrong arguments")
                        left, right = map(float, words[1:3])
                        candidate = dict(mode=0 if action == "hold" else 1,
                                         center_left=left, center_right=right)
                        duration = 0 if action == "hold" else float(words[3])
                        if action == "manual" and not 0 < duration <= 30:
                            raise ValueError("duration must be 0..30 seconds")
                        control_payload(lease, **candidate)
                        desired = candidate
                        motion_until = now + duration if duration else 0
                    elif action == "flap":
                        if len(words) != 9:
                            raise ValueError("wrong arguments")
                        frequency, left, right, center_left, center_right, phase, differential, duration = map(float, words[1:])
                        if not 0 < duration <= 30:
                            raise ValueError("duration must be 0..30 seconds")
                        candidate = dict(mode=2, frequency=frequency, left=left, right=right,
                                         center_left=center_left, center_right=center_right,
                                         phase=phase, differential=differential)
                        control_payload(lease, **candidate)
                        desired = candidate
                        motion_until = now + duration
                    else:
                        print("Unknown command")
                except (ValueError, OverflowError, struct.error) as error:
                    print("Invalid command:", error)
        finally:
            try:
                send(ESTOP, target_session=0)
            finally:
                if log:
                    log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--log", help="Append telemetry as JSONL")
    args = parser.parse_args()
    if args.list:
        from serial.tools.list_ports import comports
        for port in comports():
            print(port.device, port.description)
    elif args.port:
        run(args)
    else:
        parser.error("use --list or --port COMx")


if __name__ == "__main__":
    main()
