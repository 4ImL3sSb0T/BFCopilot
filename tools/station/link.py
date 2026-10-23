"""Console transport with prompt-delimited replies.

Two links carry the same console commands:

- Wi-Fi/TCP, the preferred path when the board is associated and reachable;
- the ESP32-C6 USB-Serial-JTAG virtual COM port, always available, used when Wi-Fi is
  unconfigured, out of range, or otherwise unreachable.

`Link` in auto mode tries Wi-Fi first and falls back to the serial port. The serial REPL
echoes the command it reads, so serial replies have that echo line stripped.
"""

import socket
import threading
import time

import serial
from serial.tools import list_ports

# The TCP console writes "hw> " itself, but the esp_console REPL appends a space to the
# configured prompt, so over USB the prompt is "hw>  " (two spaces). Match both.
PROMPTS = (b"hw>  ", b"hw> ", b"bf>  ", b"bf> ")
METHODS = ("auto", "tcp", "serial")
ESP32_USB_SERIAL_JTAG_VID = 0x303A
SERIAL_BAUDRATE = 115200


def local_prefixes():
    """IPv4 /24 prefixes worth scanning, hotspot subnet first."""
    prefixes = []
    try:
        for address in socket.gethostbyname_ex(socket.gethostname())[2]:
            if address.startswith("127."):
                continue
            prefix = address.rsplit(".", 1)[0]
            if prefix not in prefixes:
                prefixes.append(prefix)
    except OSError:
        pass
    hotspot = "192.168.137"
    if hotspot in prefixes:
        prefixes.remove(hotspot)
    return [hotspot] + prefixes


def _sweep(port, timeout, workers):
    targets = [f"{prefix}.{last}" for prefix in local_prefixes() for last in range(1, 255)]
    found = []
    lock = threading.Lock()
    cursor = [0]

    def worker():
        while True:
            with lock:
                if found or cursor[0] >= len(targets):
                    return
                target = targets[cursor[0]]
                cursor[0] += 1
            try:
                with socket.create_connection((target, port), timeout=timeout) as connection:
                    banner = bytearray()
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline and len(banner) < 4096:
                        chunk = connection.recv(64)
                        if not chunk:
                            break
                        banner.extend(chunk)
                        if banner.endswith(PROMPTS):
                            break
                    if not banner.endswith(PROMPTS):
                        continue
            except OSError:
                continue
            with lock:
                found.append(target)

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return found[0] if found else None


def discover(port, timeout=0.3, workers=64):
    """Find the board's TCP console.

    The hotspot hands addresses out over DHCP and the board takes a different one after
    every reset, so the station looks for it rather than being told. One dropped SYN loses
    the whole sweep, so an empty result is retried once instead of being reported as
    "no board".
    """
    for _ in range(2):
        host = _sweep(port, timeout, workers)
        if host:
            return host
    return None


def open_serial_port(device, baudrate=SERIAL_BAUDRATE, timeout=0.2):
    """Open the USB-Serial-JTAG console without resetting the board.

    pyserial asserts DTR and RTS on open by default. On the ESP32-C6 USB-Serial-JTAG
    peripheral RTS drives EN and DTR drives the boot strap, so asserting them reboots the
    chip (or drops it into download mode) every time the tool connects. The states have to
    be stored before open(): pyserial puts them into the DCB it applies while opening, so
    setting them afterwards is too late.

    Kept as a module function so tests can hand the link a fake transport.
    """
    connection = serial.Serial()
    connection.port = device
    connection.baudrate = baudrate
    connection.timeout = timeout
    connection.write_timeout = 2
    connection.dtr = False
    connection.rts = False
    connection.open()
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
    """Serial ports pyserial can see, best candidate first.

    Bluetooth SPP ports are dropped: no ESP32-C6 board shows up there, and opening one
    with no peer attached blocks for seconds.
    """
    ports = [port for port in list_ports.comports()
             if "BTHENUM" not in (port.hwid or "").upper()]
    return [port.device for port in sorted(ports, key=_rank)]


def _probe_serial(device, timeout):
    try:
        with open_serial_port(device) as connection:
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


def discover_serial(timeout=0.6):
    """Find the board by asking every serial port for its console prompt.

    The board is a USB device, so the port name follows the enumeration and can change
    between plug-ins. Opening a port is harmless: the console only prints a new prompt.
    """
    for device in available_ports():
        if _probe_serial(device, timeout):
            return device
    return None


class Link:
    def __init__(self, host=None, port=3333, serial_port=None, method="auto"):
        if method not in METHODS:
            raise ValueError("连接方式必须是 auto、tcp 或 serial")
        self.host = host
        self.port = port
        self.serial_port = serial_port
        self.method = method
        self.transport = None
        self._socket = None
        self._serial = None
        self._lock = threading.Lock()

    def close(self):
        connection, self._socket = self._socket, None
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        serial_connection, self._serial = self._serial, None
        if serial_connection is not None:
            try:
                serial_connection.close()
            except (OSError, serial.SerialException):
                pass
        self.transport = None

    def set_target(self, host, port, serial_port=None, method="auto"):
        port = int(port)
        if not 1 <= port <= 65535:
            raise ValueError("端口必须在 1–65535 之间")
        if method not in METHODS:
            raise ValueError("连接方式必须是 auto、tcp 或 serial")
        with self._lock:
            self.close()
            self.host = (host or "").strip() or None
            self.port = port
            self.serial_port = (serial_port or "").strip() or None
            self.method = method

    def describe(self):
        if self.transport == "tcp":
            return f"{self.host}:{self.port}"
        if self.transport == "serial":
            return f"{self.serial_port}（USB 串口）"
        return "未连接"

    def _read_reply(self, connection, transport, timeout):
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if transport == "tcp":
                try:
                    chunk = connection.recv(4096)
                except TimeoutError:
                    continue
                if not chunk:
                    raise ConnectionError("控制台连接已断开")
            else:
                try:
                    chunk = connection.read(connection.in_waiting or 1)
                except (OSError, serial.SerialException) as error:
                    raise ConnectionError(f"控制台串口已断开: {error}") from error
                if not chunk:
                    continue
            data.extend(chunk)
            for prompt in PROMPTS:
                if data.endswith(prompt):
                    return bytes(data[:-len(prompt)]).decode("utf-8", "replace")
            if len(data) > 2 * 1024 * 1024:
                raise ValueError("控制台响应超过 2 MiB")
        raise TimeoutError("等待控制台提示符超时，响应可能不完整")

    @staticmethod
    def _strip_echo(reply, line):
        """Drop the command the serial REPL echoed back before running it."""
        lines = reply.splitlines(keepends=True)
        for index, text in enumerate(lines):
            if text.strip() == line:
                del lines[index]
                break
        return "".join(lines)

    def connect(self):
        with self._lock:
            self._connect()
            return self.describe()

    def _connect(self):
        if self._socket is not None or self._serial is not None:
            return
        errors = []
        if self.method in ("auto", "tcp"):
            try:
                self._connect_tcp()
                return
            except (OSError, ValueError) as error:
                errors.append(f"Wi-Fi/TCP: {error}")
                if self.method == "tcp":
                    raise
        if self.method in ("auto", "serial"):
            try:
                self._connect_serial()
                return
            except (OSError, ValueError) as error:
                if self.method == "serial":
                    raise
                errors.append(f"串口: {error}")
        raise ValueError("；".join(errors) or "未找到开发板")

    def _connect_tcp(self):
        if not self.host:
            self.host = discover(self.port)
        if not self.host:
            raise ValueError("未找到开发板，请填写 IP 或重新搜索")
        connection = socket.create_connection((self.host, self.port), timeout=2)
        connection.settimeout(0.2)
        try:
            self._read_reply(connection, "tcp", 2)
        except Exception:
            connection.close()
            raise
        self._socket = connection
        self.transport = "tcp"

    def _connect_serial(self):
        if not self.serial_port:
            self.serial_port = discover_serial()
        if not self.serial_port:
            raise ValueError("未找到开发板串口，请选择端口或重新搜索")
        connection = open_serial_port(self.serial_port)
        try:
            connection.reset_input_buffer()
            # The REPL's prompt may already have been consumed by a monitor, so ask for a
            # fresh one with an empty line instead of assuming anything is buffered.
            connection.write(b"\n")
            connection.flush()
            self._read_reply(connection, "serial", 2)
        except serial.SerialTimeoutException as error:
            connection.close()
            raise ConnectionError(
                "串口写入超时：这个 USB 口没有在读命令。台架固件和飞控固件都把控制台放在"
                " USB-Serial-JTAG 上；若仍无响应，确认烧录的固件启用了 esp_console。"
            ) from error
        except Exception:
            connection.close()
            raise
        self._serial = connection
        self.transport = "serial"

    def run(self, line):
        if not line.strip() or any(character in line for character in ("\n", "\r", "\0")):
            raise ValueError("请输入单条控制台命令")
        line = line.strip()
        with self._lock:
            try:
                self._connect()
                timeout = 35 if "stream" in line.split() else 8
                if self.transport == "tcp":
                    connection = self._socket
                    connection.sendall(line.encode("utf-8") + b"\n")
                    return self._read_reply(connection, "tcp", timeout)
                connection = self._serial
                connection.write(line.encode("utf-8") + b"\n")
                connection.flush()
                reply = self._read_reply(connection, "serial", timeout)
                return self._strip_echo(reply, line)
            except (OSError, ValueError):
                self.close()
                raise
