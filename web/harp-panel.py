#!/usr/bin/env python3
"""HARP web panel sidecar.

Serves the device front panel + protocol inspector over HTTP, forwarding
to harp-deviced's panel API (line-JSON over a Unix socket). HTTP
robustness — concurrent clients, keep-alive, slow readers, SSE — lives
here; the C daemon stays dependency-free and single-purpose.

usage: harp-panel.py [SOCKET_PATH] [HTTP_PORT]
"""
import http.server
import json
import os
import socket
import socketserver
import sys
import threading
import time
import urllib.parse

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/harp-panel.sock"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
ROOT = os.path.dirname(os.path.abspath(__file__))


class Daemon:
    """One persistent connection to the panel API, mutex-serialized."""

    def __init__(self):
        self.lock = threading.Lock()
        self.f = None

    def cmd(self, line: str) -> str:
        with self.lock:
            for _ in range(2):  # one reconnect attempt
                try:
                    if self.f is None:
                        s = socket.socket(socket.AF_UNIX)
                        s.settimeout(2)
                        s.connect(SOCK)
                        self.f = s.makefile("rwb")
                    self.f.write(line.encode() + b"\n")
                    self.f.flush()
                    r = self.f.readline()
                    if not r:
                        raise ConnectionError
                    return r.decode().strip()
                except Exception:
                    try:
                        self.f.close()
                    except Exception:
                        pass
                    self.f = None
            return '{"error":"device daemon unreachable"}'


DAEMON = Daemon()


def snapshot() -> str:
    return json.dumps({
        "params": json.loads(DAEMON.cmd("params")),
        "refs": json.loads(DAEMON.cmd("refs")),
        "counters": json.loads(DAEMON.cmd("counters")),
    })


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, code, ctype, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path == "/":
            with open(os.path.join(ROOT, "panel.html"), "rb") as f:
                self._send(200, "text/html; charset=utf-8", f.read())
        elif u.path in ("/api/params", "/api/refs", "/api/counters"):
            self._send(200, "application/json",
                       DAEMON.cmd(u.path.rsplit("/", 1)[1]).encode())
        elif u.path == "/api/knob":
            pid = q.get("id", [""])[0]
            val = q.get("value", [""])[0]
            self._send(200, "application/json",
                       DAEMON.cmd(f"knob {pid} {val}").encode())
        elif u.path == "/api/revert":
            name = q.get("name", [""])[0]
            self._send(200, "application/json",
                       DAEMON.cmd(f"revert {name}").encode())
        elif u.path == "/api/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            last = None
            try:
                while True:
                    snap = snapshot()
                    if snap != last:
                        self.wfile.write(f"data: {snap}\n\n".encode())
                        last = snap
                    else:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    time.sleep(0.25)
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self._send(404, "text/plain", b"not found\n")

    do_POST = do_GET


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    print(f"harp-panel: http://0.0.0.0:{PORT}/ -> {SOCK}", flush=True)
    Server(("0.0.0.0", PORT), Handler).serve_forever()
