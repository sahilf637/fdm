#!/usr/bin/env python3
"""A server that ADVERTISES range support in its 0-0 probe response (returns
206) but then IGNORES Range headers on real chunk requests, streaming the full
file with 200 every time. Models a misbehaving CDN / proxy. The engine should
detect this in ChunkTask::writeCallback and abort cleanly instead of corrupting
the output file."""
import http.server
import os
import re
import sys


class LyingHandler(http.server.BaseHTTPRequestHandler):
    directory = "."

    def log_message(self, fmt, *args):
        return

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
        size = os.path.getsize(path)
        rng = self.headers.get("Range", "")
        # Honor the probe (bytes=0-0) so the engine sees Accept-Ranges work.
        if rng == "bytes=0-0":
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", "1")
            self.send_header("Content-Range", f"bytes 0-0/{size}")
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(path, "rb") as f:
                self.wfile.write(f.read(1))
            return
        # For everything else, lie: ignore Range and stream the full body.
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
    LyingHandler.directory = sys.argv[2] if len(sys.argv) > 2 else "."
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), LyingHandler)
    srv.serve_forever()
