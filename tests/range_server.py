#!/usr/bin/env python3
"""Minimal test HTTP server that advertises Accept-Ranges: bytes and serves
byte-range requests. Python's stdlib http.server does not, so we need this for
exercising the engine's multi-chunk path."""
import http.server
import os
import re
import sys
from urllib.parse import unquote


class RangeHandler(http.server.BaseHTTPRequestHandler):
    directory = "."

    def log_message(self, fmt, *args):
        return  # silence stderr per-request logs

    def _resolve(self):
        rel = self.path.lstrip("/").split("?", 1)[0]
        full = os.path.join(self.directory, rel)
        if not os.path.isfile(full):
            self.send_error(404)
            return None
        return full

    def _maybe_send_disposition(self):
        # Tests opt in via ?dispo=<percent-encoded-header-value>. The value
        # is forwarded verbatim so tests can exercise quoted, unquoted, and
        # RFC 5987 filename* forms.
        q = self.path.split("?", 1)
        if len(q) != 2:
            return
        params = dict(p.split("=", 1) for p in q[1].split("&") if "=" in p)
        if "dispo" in params:
            self.send_header("Content-Disposition", unquote(params["dispo"]))

    def do_HEAD(self):
        path = self._resolve()
        if not path:
            return
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Accept-Ranges", "bytes")
        self._maybe_send_disposition()
        self.end_headers()

    def do_GET(self):
        path = self._resolve()
        if not path:
            return
        size = os.path.getsize(path)
        rng = self.headers.get("Range")
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
            self._maybe_send_disposition()
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
            self._maybe_send_disposition()
            self.end_headers()
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)


if __name__ == "__main__":
    port = int(sys.argv[1])
    RangeHandler.directory = sys.argv[2] if len(sys.argv) > 2 else "."
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), RangeHandler)
    srv.serve_forever()
