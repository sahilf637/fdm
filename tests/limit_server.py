#!/usr/bin/env python3
# Range-capable server that allows only LIMIT concurrent transfers and replies
# 429 to anything beyond that -- models a rate-limiting CDN. Records the peak
# concurrency it ever served into <root>/maxconc.txt. Unknown paths -> 404.
#
# Usage: limit_server.py <port> <root> [limit]   (limit defaults to 2)
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
ROOT = sys.argv[2]
LIMIT = int(sys.argv[3]) if len(sys.argv) > 3 else 2

_lock = threading.Lock()
_active = 0
_peak = 0


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        global _active, _peak
        path = os.path.join(ROOT, self.path.lstrip("/").split("?")[0])
        if not os.path.isfile(path):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        with _lock:
            if _active >= LIMIT:
                self.send_response(429)  # over the concurrency limit
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            _active += 1
            _peak = max(_peak, _active)
            with open(os.path.join(ROOT, "maxconc.txt"), "w") as f:
                f.write(str(_peak))

        try:
            with open(path, "rb") as fh:
                data = fh.read()
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes="):
                lo, _, hi = rng[len("bytes="):].partition("-")
                start = int(lo) if lo else 0
                end = int(hi) if hi else len(data) - 1
                body = data[start:end + 1]
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
            else:
                body = data
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
            # Throttle so connections overlap and the limit actually bites.
            view = memoryview(body)
            piece = 64 * 1024
            for i in range(0, len(view), piece):
                self.wfile.write(view[i:i + piece])
                time.sleep(0.005)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with _lock:
                _active -= 1

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
