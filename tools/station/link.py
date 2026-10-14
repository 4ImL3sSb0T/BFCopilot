"""Serialized console transport with prompt-delimited replies."""

import socket
import threading
import time

PROMPTS = (b"hw> ", b"bf> ")

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
    """Find the board by its console port.

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

class Link:
    def __init__(self, host=None, port=3333):
        self.host = host
        self.port = port
        self._socket = None
        self._lock = threading.Lock()

    def close(self):
        connection, self._socket = self._socket, None
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()

    def set_target(self, host, port):
        port = int(port)
        if not 1 <= port <= 65535:
            raise ValueError("端口必须在 1–65535 之间")
        with self._lock:
            self.close()
            self.host = host.strip() or None
            self.port = port

    def _read_reply(self, connection, timeout):
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = connection.recv(4096)
            except TimeoutError:
                continue
            if not chunk:
                raise ConnectionError("控制台连接已断开")
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
        return f"{self.host}:{self.port}"

    def _connect(self):
        if self._socket is not None:
            return
        if not self.host:
            self.host = discover(self.port)
        if not self.host:
            raise ValueError("未找到开发板，请填写 IP 或重新搜索")
        connection = socket.create_connection((self.host, self.port), timeout=2)
        connection.settimeout(0.2)
        try:
            self._read_reply(connection, 2)
        except Exception:
            connection.close()
            raise
        self._socket = connection

    def run(self, line):
        if not line.strip() or any(character in line for character in ("\n", "\r", "\0")):
            raise ValueError("请输入单条控制台命令")
        line = line.strip()
        with self._lock:
            try:
                self._connect()
                connection = self._socket
                connection.sendall(line.encode("utf-8") + b"\n")
                return self._read_reply(connection, 8)
            except (OSError, ValueError):
                self.close()
                raise
