"""Wi-Fi UDP 传输。

每个数据报承载一个 60 字节帧，不做 COBS；飞控把 STATUS/ACK 回复到最近一次
有效数据报的源地址和端口，所以电脑这边绑定临时端口即可。
"""

from __future__ import annotations

import socket
import time

from .protocol import DISCOVER, DISCOVER_REPLY


def local_prefixes() -> list[str]:
    prefixes: list[str] = []
    try:
        for address in socket.gethostbyname_ex(socket.gethostname())[2]:
            if address.startswith("127."):
                continue
            prefix = address.rsplit(".", 1)[0]
            if prefix not in prefixes:
                prefixes.append(prefix)
    except OSError:
        pass
    hotspot = "192.168.4"
    if hotspot in prefixes:
        prefixes.remove(hotspot)
    return [hotspot] + prefixes


class UdpTransport:
    def __init__(self) -> None:
        self._socket: socket.socket | None = None
        self._address: tuple[str, int] | None = None
        self.tx = 0
        self.rx = 0
        self.bad = 0
        self.last_rx = 0.0
        self.last_error = ""

    @property
    def address(self) -> tuple[str, int] | None:
        return self._address

    def open(self) -> None:
        if self._socket is not None:
            return
        connection = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        connection.bind(("", 0))
        connection.setblocking(False)
        self._socket = connection
        self.tx = self.rx = self.bad = 0
        self.last_rx = 0.0
        self.last_error = ""

    def close(self) -> None:
        connection, self._socket = self._socket, None
        self._address = None
        if connection is not None:
            try:
                connection.close()
            except OSError:
                pass

    def set_peer(self, host: str, port: int) -> None:
        host = host.strip()
        if not host:
            raise ValueError("飞控 IP 不能为空")
        if not 1 <= int(port) <= 65535:
            raise ValueError("端口必须在 1..65535")
        self._address = (host, int(port))

    def send(self, raw: bytes) -> bool:
        if self._socket is None or self._address is None:
            return False
        try:
            self._socket.sendto(raw, self._address)
            self.tx += 1
            return True
        except OSError as error:
            self.last_error = str(error)
            return False

    def receive(self, limit: int = 64) -> list[tuple[bytes, tuple[str, int]]]:
        if self._socket is None:
            return []
        messages: list[tuple[bytes, tuple[str, int]]] = []
        for _ in range(limit):
            try:
                data, address = self._socket.recvfrom(2048)
            except (BlockingIOError, InterruptedError):
                break
            except OSError as error:
                self.last_error = str(error)
                break
            self.rx += 1
            self.last_rx = time.monotonic()
            messages.append((data, address))
        return messages

    def age(self) -> float:
        return 0.0 if not self.last_rx else time.monotonic() - self.last_rx


def discover(port: int = 3335, timeout: float = 1.0, probes: int = 3) -> list[str]:
    """广播查找飞控 IP，返回应答的地址（去重、保持出现顺序）。"""
    found: list[str] = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        connection.bind(("", 0))
        connection.settimeout(0.15)
        targets = ["255.255.255.255"]
        for prefix in local_prefixes():
            target = f"{prefix}.255"
            if target not in targets:
                targets.append(target)
        deadline = time.monotonic() + timeout
        for _ in range(probes):
            for target in targets:
                try:
                    connection.sendto(DISCOVER, (target, int(port)))
                except OSError:
                    continue
            try:
                while True:
                    data, address = connection.recvfrom(512)
                    if data.startswith(DISCOVER_REPLY) and address[0] not in found:
                        found.append(address[0])
            except (socket.timeout, BlockingIOError):
                pass
            if found or time.monotonic() >= deadline:
                break
    return found
