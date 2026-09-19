#!/usr/bin/env python3
"""omr-probe-server: measurement endpoints for the OMR survey rig.

  UDP echo  (default :8621)  -- echoes every datagram back unchanged
                                (latency / loss / jitter probes)
  HTTP      (default :8622)  -- GET  /down?bytes=N   streams N bytes
                                POST /up             discards the body,
                                                     answers {"received":N}
                                GET  /ping           {"ok":true,"t":...}
No state, no auth: only meant for a rig whose carriers are being surveyed.
"""
import argparse, json, os, socket, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

CHUNK = os.urandom(64 * 1024)  # incompressible payload
MAX_DOWN = 200 * 1024 * 1024


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):  # quiet
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/ping":
            return self._json(200, {"ok": True, "t": time.time()})
        if u.path == "/down":
            try:
                n = int(parse_qs(u.query).get("bytes", ["5000000"])[0])
            except ValueError:
                return self._json(400, {"error": "bad bytes"})
            n = max(0, min(n, MAX_DOWN))
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(n))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            left = n
            try:
                while left > 0:
                    k = min(left, len(CHUNK))
                    self.wfile.write(CHUNK[:k])
                    left -= k
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        self._json(404, {"error": "not found"})

    def do_POST(self):
        u = urlparse(self.path)
        if u.path != "/up":
            return self._json(404, {"error": "not found"})
        n = int(self.headers.get("Content-Length", "0") or 0)
        left, got = n, 0
        try:
            while left > 0:
                b = self.rfile.read(min(left, 256 * 1024))
                if not b:
                    break
                got += len(b)
                left -= len(b)
        except (BrokenPipeError, ConnectionResetError):
            pass
        self._json(200, {"received": got, "t": time.time()})

    do_PUT = do_POST


def udp_echo(bind, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind, port))
    while True:
        try:
            data, addr = s.recvfrom(2048)
            s.sendto(data, addr)
        except OSError:
            continue


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--udp-port", type=int, default=8621)
    ap.add_argument("--http-port", type=int, default=8622)
    a = ap.parse_args()
    threading.Thread(target=udp_echo, args=(a.bind, a.udp_port), daemon=True).start()
    srv = ThreadingHTTPServer((a.bind, a.http_port), Handler)
    srv.daemon_threads = True
    print(f"omr-probe-server: udp echo :{a.udp_port}, http :{a.http_port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
