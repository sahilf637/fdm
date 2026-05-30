#!/usr/bin/env python3
# Minimal range-capable server that ALSO records the request headers it
# receives into <root>/received_headers.txt. Used to prove the engine forwards
# caller-supplied headers (Cookie / Referer / User-Agent) onto the wire.
#
# Usage: header_echo_server.py <port> <root>
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1])
ROOT = sys.argv[2]


class Handler(BaseHTTPRequestHandler):
    def _record(self):
        with open(os.path.join(ROOT, "received_headers.txt"), "a") as f:
            for key, value in self.headers.items():
                f.write(f"{key}: {value}\n")
            f.write("---\n")

    def do_GET(self):
        self._record()
        path = os.path.join(ROOT, self.path.lstrip("/").split("?")[0])
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError:
            self.send_response(404)
            self.end_headers()
            return

        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            spec = rng[len("bytes="):]
            lo, _, hi = spec.partition("-")
            start = int(lo) if lo else 0
            end = int(hi) if hi else len(data) - 1
            chunk = data[start:end + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(len(chunk)))
            self.end_headers()
            self.wfile.write(chunk)
        else:
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
