"""Serial console transport over the board's USB-Serial-JTAG port."""

import threading
import time

import serial
from serial.tools import list_ports

BAUDRATE = 115200
PROMPTS = (b"hw> ", b"bf> ")
ESP32_USB_SERIAL_JTAG_VID = 0x303A


def open_port(device, baudrate=BAUDRATE, timeout=0.2):
    """Open a console port.

    Kept as a module function so tests can hand the link a fake transport.
    DTR/RTS are dropped after opening: the ESP32-C6 USB-Serial-JTAG reset circuit
    watches those lines, and a tool that only wants the console must not reset the board.
    """
    connection = serial.Serial(device, baudrate, timeout=timeout, write_timeout=2)
    try:
        connection.dtr = False
        connection.rts = False
    except (OSError, serial.SerialException):
        pass
    return connection


def _rank(port):
    """Likely boards first: Espressif's USB-Serial-JTAG, then anything else."""
    description = (port.description or "").lower()
    if port.vid == ESP32_USB_SERIAL_JTAG_VID:
        return 0
    if "jtag" in description or "serial" in description:
        return 1
    return 2


def available_ports():
    """Ports pyserial can see, best candidate first.

    Bluetooth SPP ports are dropped: no ESP32-C6 board shows up there, and opening one
    with no peer attached blocks for seconds.
    """
    ports = [port for port in list_ports.comports()
             if "BTHENUM" not in (port.hwid or "").upper()]
    return [port.device for port in sorted(ports, key=_rank)]


def _probe(device, timeout):
    try:
        with open_port(device) as connection:
            connection.reset_input_buffer()
            connection.write(b"\n")
            connection.flush()
            data = bytearray()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                chunk = connection.read(connection.in_waiting or 1)
                if not chunk:
                    continue
                data.extend(chunk)
                if data.endswith(PROMPTS):
                    return True
            return False
    except (OSError, serial.SerialException):
        return False


def discover(timeout=0.6):
    """Find the board by asking every serial port for its console prompt.

    The board is a USB device, so the port name follows the enumeration and can change
    between plug-ins. Opening a port is harmless: the console only prints a new prompt.
    """
    for device in available_ports():
        if _probe(device, timeout):
            return device
    return None


class Link:
    def __init__(self, port=None, baudrate=BAUDRATE):
        self.port = port
        self.baudrate = baudrate
        self._serial = None
        self._lock = threading.Lock()

    def close(self):
        connection, self._serial = self._serial, None
        if connection is not None:
            try:
                connection.close()
            except (OSError, serial.SerialException):
                pass

    def set_target(self, port):
        with self._lock:
            self.close()
            self.port = (port or "").strip() or None

    def _read_reply(self, connection, timeout):
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = connection.read(connection.in_waiting or 1)
            except (OSError, serial.SerialException) as error:
                raise ConnectionError(f"串口连接已断开: {error}") from error
            if not chunk:
                continue
            data.extend(chunk)
            for prompt in PROMPTS:
                if data.endswith(prompt):
                    return bytes(data[:-len(prompt)]).decode("utf-8", "replace")
            if len(data) > 2 * 1024 * 1024:
                raise ValueError("控制台响应超过 2 MiB")
        raise TimeoutError("等待控制台提示符超时，响应可能不完整")

    def connect(self):
        with self._lock:
            self._connect()
            return str(self.port)

    def _connect(self):
        if self._serial is not None:
            return
        if not self.port:
            self.port = discover()
        if not self.port:
            raise ValueError("未找到开发板串口，请选择端口或重新搜索")
        connection = open_port(self.port, self.baudrate)
        try:
            connection.reset_input_buffer()
            # The REPL's prompt may already have been consumed by a monitor, so ask for a
            # fresh one with an empty line instead of assuming anything is buffered.
            connection.write(b"\n")
            connection.flush()
            self._read_reply(connection, 2)
        except Exception:
            connection.close()
            raise
        self._serial = connection

    @staticmethod
    def _strip_echo(reply, line):
        """Drop the command the REPL echoed back before running it."""
        lines = reply.splitlines(keepends=True)
        for index, text in enumerate(lines):
            if text.strip() == line:
                del lines[index]
                break
        return "".join(lines)

    def run(self, line):
        if not line.strip() or any(character in line for character in ("\n", "\r", "\0")):
            raise ValueError("请输入单条控制台命令")
        line = line.strip()
        with self._lock:
            try:
                self._connect()
                connection = self._serial
                connection.write(line.encode("utf-8") + b"\n")
                connection.flush()
                reply = self._read_reply(connection, 35 if "stream" in line.split() else 8)
            except (OSError, ValueError):
                self.close()
                raise
        return self._strip_echo(reply, line)
