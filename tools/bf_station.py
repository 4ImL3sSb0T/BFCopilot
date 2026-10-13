#!/usr/bin/env python3
"""BFCopilot hardware-test station: a local web UI over the bench firmware's console.

The bench firmware runs no flight logic - it serves a hardware console over TCP, and the
station is a thin, friendly front end for it: per-device test buttons, a register poke
form, a live view of whichever command you point it at, and the raw console.

    uv run --project tools python tools/bf_station.py --open
"""

import argparse
import json
import os
import socket
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The board marks the end of a command's output with its prompt.
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


def discover(port, timeout=0.3, workers=64):
    """Find the board by its console port.

    The hotspot hands addresses out over DHCP and the board takes a different one after
    every reset, so the station looks for it rather than being told.
    """
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
                with socket.create_connection((target, port), timeout=timeout):
                    pass
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


class Link:
    """The board's console over TCP, and finding the board in the first place."""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self._socket = None
        self._lock = threading.Lock()

    def address(self):
        if not self.host:
            self.host = discover(self.port)
        return self.host

    def status(self):
        return {"host": self.host, "port": self.port, "reachable": self._socket is not None}

    def _connect(self):
        if self._socket is not None:
            return
        host = self.address()
        if not host:
            raise ValueError("no board found on the network")
        self._socket = socket.create_connection((host, self.port), timeout=5)
        self._socket.settimeout(0.2)
        self._socket.recv(4096)  # the prompt the board sends on connect

    def run(self, line):
        """Send one command and return its output, prompt stripped."""
        with self._lock:
            data = b""
            try:
                self._connect()
                self._socket.sendall(line.encode() + b"\n")
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    try:
                        chunk = self._socket.recv(4096)
                    except TimeoutError:
                        continue
                    except OSError as error:
                        self._socket = None
                        raise ValueError(f"console dropped: {error}") from error
                    if not chunk:
                        self._socket = None
                        break
                    data += chunk
                    if data.endswith(PROMPTS):
                        break
            except OSError as error:
                self._socket = None
                raise ValueError(f"board unreachable: {error}") from error
            text = data.decode("utf-8", "replace")
            for prompt in PROMPTS:
                if text.endswith(prompt.decode()):
                    return text[: -len(prompt)]
            return text


class Handler(BaseHTTPRequestHandler):
    link = None
    page = b""

    def log_message(self, *args):
        pass

    def _reply(self, code, body, content_type="application/json"):
        payload = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self._reply(200, self.page, "text/html; charset=utf-8")
            return
        if self.path == "/link":
            self._reply(200, json.dumps(self.link.status()))
            return
        self._reply(404, json.dumps({"error": "not found"}))

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            self._reply(400, json.dumps({"ok": False, "message": "bad JSON"}))
            return
        if self.path != "/console":
            self._reply(404, json.dumps({"ok": False, "message": "not found"}))
            return
        try:
            output = self.link.run(body.get("line", ""))
        except ValueError as error:
            self._reply(200, json.dumps({"ok": False, "message": str(error)}))
            return
        self._reply(200, json.dumps({"ok": True, "output": output}))


def main():
    parser = argparse.ArgumentParser(description="BFCopilot hardware test station")
    parser.add_argument("--host", help="board address; scanned for when omitted")
    parser.add_argument("--port", type=int, default=3333,
                        help="the board's console port (CONFIG_BF_BENCH_PORT)")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--open", action="store_true", help="open the browser")
    args = parser.parse_args()

    host = args.host
    if not host:
        print("looking for a board on the local network ...")
        host = discover(args.port)
        print(f"board at {host}" if host else "no board yet - the station keeps looking")

    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "station.html"), "rb") as handle:
        Handler.page = handle.read()
    Handler.link = Link(host, args.port)

    server = ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler)
    url = f"http://127.0.0.1:{args.http_port}/"
    print(f"hardware test station on {url}  ->  {host or 'searching'}:{args.port}")
    if args.open:
        threading.Thread(target=lambda: (time.sleep(0.5), webbrowser.open(url)),
                         daemon=True).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
