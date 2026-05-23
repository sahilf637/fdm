#!/usr/bin/env python3
"""Range-supporting HTTP server that fails each unique (path, Range) request
once with 503, then serves it normally on subsequent attempts. Used to verify
the engine's retry-on-failure path."""
import http.server
import os
import re
import sys


class FlakyHandler(http.server.BaseHTTPRequestHandler):
    directory = "."
    failed_once = set()

    def log_message(self, fmt, *args):
        return  # silence stderr

    def _resolve(self):
        rel = self.path.lstrip("/")
        full = os.path.join(self.directory, rel)
        if not os.path.isfile(full):
            self.send_error(404)
            return None
        return full

    def do_HEAD(self):
        path = self._resolve()
        if not path:
            return
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        path = self._resolve()
        if not path:
            return
        rng = self.headers.get("Range", "")
        # Skip the flake for the engine's single-byte probe ("bytes=0-0") so
        # the test exercises chunk retry, not probe retry.
        if rng != "bytes=0-0":
            key = (self.path, rng)
            if key not in FlakyHandler.failed_once:
                FlakyHandler.failed_once.add(key)
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

        size = os.path.getsize(path)
        if rng:
            m = re.match(r"bytes=(\d+)-(\d*)", rng)
            if not m:
                self.send_error(416)
                return
            start = int(m.group(1))
            end = int(m.group(2)) if m.group(2) else size - 1
            if start >= size or end >= size or start > end:
                self.send_error(416)
                return
            length = end - start + 1
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(length))
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(path, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(65536, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        else:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)


if __name__ == "__main__":
    port = int(sys.argv[1])
    FlakyHandler.directory = sys.argv[2] if len(sys.argv) > 2 else "."
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), FlakyHandler)
    srv.serve_forever()
