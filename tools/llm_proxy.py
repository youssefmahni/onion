#!/usr/bin/env python3
"""Recording pass-through proxy for real-API experiments (W5 baseline).

The engine points its --base-url here; every request is recorded as a
cassette line (request headers minus Authorization, body, response) and
forwarded to DeepSeek with the proxy's own key from the environment.
Standard library only; OS-assigned port via --port 0."""
import argparse
import json
import os
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


UPSTREAM = "https://api.deepseek.com"


class Recorder:
    def __init__(self, path):
        self.path = Path(path)
        self.lock = threading.Lock()

    def record(self, entry):
        with self.lock:
            with self.path.open("a", encoding="utf-8", newline="\n") as out:
                out.write(json.dumps(entry, ensure_ascii=False,
                                     separators=(",", ":")) + "\n")


class Proxy(BaseHTTPRequestHandler):
    recorder = None

    def log_message(self, *_args):
        return

    def _forward(self, method):
        size = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(size).decode("utf-8", "replace") if size else ""
        target = UPSTREAM + self.path
        request = urllib.request.Request(target, data=body.encode() if body else None,
                                         method=method)
        key = os.environ.get("DEEPSEEK_API_KEY", "")
        if key:
            request.add_header("Authorization", "Bearer " + key)
        request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                status = response.status
                payload = response.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as error:
            status = error.code
            payload = error.read().decode("utf-8", "replace")
        except Exception as error:
            status = 502
            payload = json.dumps({"error": str(error)})
        incoming = {k: v for k, v in self.headers.items() if k.lower() != "authorization"}
        Proxy.recorder.record({"ts": round(time.time(), 3), "method": method,
                               "path": self.path,
                               "headers": incoming, "body": body,
                               "status": status, "response": payload})
        encoded = payload.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def do_POST(self):
        self._forward("POST")

    def do_GET(self):
        self._forward("GET")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", required=True, type=Path)
    parser.add_argument("--port", default=0, type=int)
    args = parser.parse_args()
    Proxy.recorder = Recorder(args.record)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Proxy)
    print(server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
