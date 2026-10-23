#!/usr/bin/env python3
"""USB-Serial-JTAG console front end for the flight and bench firmware.

The esp_console REPL lives on the board's USB-Serial-JTAG port. `idf.py monitor`
does not reliably accept typed input on that port, so this tool opens it itself
without resetting the board, sends one line, and waits for reply silence.

    uv run --project tools python tools/bf_cli.py                      # interactive
    uv run --project tools python tools/bf_cli.py "status"
    uv run --project tools python tools/bf_cli.py "param get sensors.vbat_div_num"
    uv run --project tools python tools/bf_cli.py --json "param set x 1"
    uv run --project tools python tools/bf_cli.py --list
    uv run --project tools python tools/bf_cli.py --serial COM4 --reset "status"

Output is the cleaned console reply: prompt fragments, the echoed command and
the firmware's `I (tick) tag: ...` log lines are removed. `--json` prints one
object per command ({"command", "ok", "output"}) so an agent can parse it.
"""

import argparse
import json
import math
import re
import sys
import time

import serial
import serial.tools.list_ports

PROMPT_TEXT = re.compile(r"(?:bf|hw)> ?")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LOGLINE = re.compile(r"^(?:[IWEVD] )?\(\d+\) [^:]+: ")
FAILURE = re.compile(
    r"(?im)\b(unrecognized command|refused|failed|unknown parameter|unknown servo|"
    r"not supported|non-zero error code|internal error|ESP_ERR_\w+)\b|^usage:"
)
ESP32_USB_SERIAL_JTAG_VID = 0x303A
MAX_COMMAND_BYTES = 158


class ConsoleError(Exception):
    pass


class ReplyTimeout(TimeoutError):
    def __init__(self, message, output=""):
        super().__init__(message)
        self.output = output


def validate_command(line):
    if any(ord(character) < 32 or ord(character) == 127 for character in line):
        raise ValueError("命令不能包含换行、控制字符或终端转义序列")
    line = line.strip()
    if not line or len(line.encode("utf-8")) > MAX_COMMAND_BYTES:
        raise ValueError(f"命令不能为空且不能超过 {MAX_COMMAND_BYTES} 个 UTF-8 字节")
    return line


def clean(raw: bytes, line: str) -> str:
    """Strip NULs, ANSI escapes, firmware log lines, prompts and the echo."""
    text = raw.decode("utf-8", "replace").replace("\x00", "").replace("\r", "")
    rows = []
    for row in text.split("\n"):
        row = ANSI.sub("", row).strip()
        if not row or LOGLINE.match(row):
            continue
        for piece in (part.strip() for part in PROMPT_TEXT.split(row)):
            if piece and piece != line and not LOGLINE.match(piece):
                rows.append(piece)
    return "\n".join(rows)


class Console:
    """One request at a time over USB-Serial-JTAG, filtering background logs.

    A leading space tolerates the observed loss of the first input byte.
    Only CR is sent because the firmware translates CR into a newline.
    """

    def __init__(self, device, reset=False):
        self.device = device
        self._desynchronized = False
        self._pending = b""
        self.port = serial.Serial()
        self.port.port = device
        self.port.baudrate = 115200
        self.port.timeout = 0.1
        self.port.write_timeout = 2
        self.port.dtr = False
        self.port.rts = False
        try:
            self.port.open()
        except (OSError, serial.SerialException) as error:
            raise ConsoleError(f"打开 {device} 失败: {error}") from error
        try:
            if reset:
                self.reset()
            else:
                self._drain(0.5)
        except Exception:
            self.close()
            raise

    def close(self):
        try:
            self.port.close()
        except (OSError, serial.SerialException):
            pass

    def reset(self):
        """Explicit USB-Serial-JTAG reset, followed by Windows re-enumeration."""
        try:
            self.port.rts = True
            self.port.dtr = False
            time.sleep(0.2)
            self.port.rts = False
            self.port.dtr = False
        finally:
            self.close()
        deadline = time.monotonic() + 8
        while True:
            try:
                self.port.open()
                break
            except (OSError, serial.SerialException) as error:
                if time.monotonic() >= deadline:
                    raise ConsoleError(f"复位后无法重连 {self.device}: {error}") from error
                time.sleep(0.2)
        self._drain(2)
        self._desynchronized = False

    def _drain(self, seconds):
        deadline = time.monotonic() + seconds
        data = bytearray(self._pending)
        while time.monotonic() < deadline:
            data.extend(self.port.read(self.port.in_waiting or 1))
        self._pending = bytes(data).rsplit(b"\n", 1)[-1]

    def _read_reply(self, timeout, line, quiet=0.45):
        """Read until the reply goes quiet for `quiet` seconds, ignoring log lines."""
        deadline = time.monotonic() + timeout
        data = bytearray(self._pending)
        self._pending = b""
        last_line = None
        output = ""
        while time.monotonic() < deadline:
            chunk = self.port.read(self.port.in_waiting or 1)
            if chunk:
                data.extend(chunk)
                complete = bytes(data[:data.rfind(b"\n") + 1])
                reply = clean(complete, line)
                if reply and reply != output:
                    output = reply
                    last_line = time.monotonic()
            if last_line is not None and time.monotonic() - last_line >= quiet:
                boundary = data.rfind(b"\n") + 1
                pending = bytes(data[boundary:])
                if not clean(pending, line):
                    self._pending = pending
                    return bytes(data[:boundary])
        raise ReplyTimeout(
            f"{self.device} 回复超时；命令可能已执行，未自动重发。"
            "如输入失灵，可显式使用 --reset 后先查询状态和参数。",
            clean(bytes(data), line),
        )

    def run(self, line, timeout=8):
        line = validate_command(line)
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout 必须是正数")
        if self._desynchronized:
            raise ConsoleError("上条命令未确认完成；请重连，必要时使用 --reset")
        try:
            waiting = self.port.in_waiting
            if waiting:
                self._pending += self.port.read(waiting)
                self._pending = self._pending.rsplit(b"\n", 1)[-1]
            payload = b" " + line.encode("utf-8") + b"\r"
            if self.port.write(payload) != len(payload):
                raise ConsoleError("串口写入不完整；命令状态未知，请重连后查询")
            return clean(self._read_reply(timeout, line), line)
        except (ConsoleError, TimeoutError, OSError, serial.SerialException):
            self._desynchronized = True
            raise


def candidate_ports():
    ports = [port for port in serial.tools.list_ports.comports()
             if "BTHENUM" not in (port.hwid or "").upper()]
    return sorted(ports, key=lambda port: 0 if port.vid == ESP32_USB_SERIAL_JTAG_VID else 1)


def open_console(device=None, reset=False):
    if device:
        return Console(device, reset=reset)
    ports = candidate_ports()
    if reset:
        raise ConsoleError("--reset 必须配合 --serial 明确指定设备")
    errors = []
    for port in ports:
        try:
            return Console(port.device)
        except (ConsoleError, TimeoutError, OSError) as error:
            errors.append(f"{port.device}: {error}")
    detail = "; ".join(errors) or "没有可用的串口"
    raise ConsoleError(f"没找到控制台: {detail}")


def print_ports():
    for port in candidate_ports():
        print(f"{port.device}\t{port.description or ''}")
    return 0


def interactive(console):
    print("已连接；:quit 或 Ctrl-D 退出", file=sys.stderr)
    while True:
        try:
            line = input("console> ")
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        line = line.strip()
        if not line:
            continue
        if line in (":q", ":quit", "exit", "quit"):
            return 0
        try:
            output = console.run(line)
        except (ConsoleError, TimeoutError, ValueError, OSError, serial.SerialException) as error:
            print(f"error: {error}", file=sys.stderr)
            if not isinstance(error, ValueError):
                return 2
            continue
        if output:
            print(output)


def main(argv=None):
    parser = argparse.ArgumentParser(description="BFCopilot USB-Serial-JTAG 控制台前端")
    parser.add_argument("commands", nargs="*", help="要执行的命令；留空进入交互模式")
    parser.add_argument("--serial", help="串口（如 COM4）；留空自动探测")
    parser.add_argument("--json", action="store_true", help="输出 JSON，供脚本/Agent 解析")
    parser.add_argument("--list", action="store_true", help="列出串口后退出")
    parser.add_argument("--reset", action="store_true", help="显式复位指定串口的板子，再重连")
    parser.add_argument("--timeout", type=float, default=8, help="单条命令等待回复的秒数")
    args = parser.parse_args(argv)

    if args.list:
        if args.json:
            print(json.dumps([{"port": port.device, "description": port.description}
                              for port in candidate_ports()], ensure_ascii=True))
            return 0
        return print_ports()

    try:
        if args.json and not args.commands:
            raise ValueError("--json 需要提供至少一条命令")
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError("--timeout 必须是正数")
        for command in args.commands:
            validate_command(command)
        console = open_console(args.serial, reset=args.reset)
    except (ConsoleError, ValueError, TimeoutError, OSError, serial.SerialException) as error:
        if args.json:
            print(json.dumps({"command": args.commands[0] if len(args.commands) == 1 else None,
                              "ok": False, "output": "", "error": str(error)}, ensure_ascii=True))
        else:
            print(f"error: {error}", file=sys.stderr)
        return 2

    try:
        if not args.commands:
            return interactive(console)
        results = []
        status = 0
        for command in args.commands:
            try:
                output = console.run(command, args.timeout)
            except (ConsoleError, TimeoutError, ValueError, OSError, serial.SerialException) as error:
                results.append({"command": command, "ok": False,
                                "output": getattr(error, "output", ""), "error": str(error)})
                status = 2
                break
            ok = not FAILURE.search(output)
            results.append({"command": command, "ok": ok, "output": output})
            if not ok:
                status = 1
        if args.json:
            payload = results[0] if len(args.commands) == 1 else results
            print(json.dumps(payload, ensure_ascii=True))
        else:
            for result in results:
                if result["output"]:
                    print(result["output"])
                if "error" in result:
                    print(f"error: {result['error']}", file=sys.stderr)
        return status
    finally:
        console.close()


if __name__ == "__main__":
    sys.exit(main())
