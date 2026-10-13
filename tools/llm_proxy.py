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


UPSTREAM = os.environ.get("ONION_PROXY_UPSTREAM", "https://api.deepseek.com")

# Budget caps (owner, 2026-10-13): 4,000 total calls and $5.00 total.
# The proxy itself refuses further /chat/completions once either cap is
# reached, using the prices from decision 0004 (peak, conservative).
CALL_CAP = int(os.environ.get("ONION_PROXY_CALL_CAP", "4000"))
USD_CAP = float(os.environ.get("ONION_PROXY_USD_CAP", "5.00"))
PRICE_IN_PER_M = 0.27   # USD per 1M input tokens, peak (0004)
PRICE_OUT_PER_M = 1.10  # USD per 1M output tokens, peak (0004)


class Budget:
    """Cumulative call/dollar counters; the history in the record file
    counts, so a restarted proxy keeps enforcing the lifetime caps."""

    def __init__(self):
        self.lock = threading.Lock()
        self.calls = 0
        self.usd = 0.0

    def load_history(self, record_path):
        path = Path(record_path)
        if not path.exists():
            return
        try:
            with path.open(encoding="utf-8") as source:
                for line in source:
                    try:
                        entry = json.loads(line)
                    except Exception:
                        continue
                    if "/chat/completions" not in entry.get("path", ""):
                        continue
                    self.calls += 1
                    self.usd += self.usage_usd(entry.get("response", "{}"))
        except OSError:
            pass

    @staticmethod
    def usage_usd(response_text):
        try:
            usage = json.loads(response_text).get("usage", {})
            tokens_in = int(usage.get("prompt_tokens", 0))
            tokens_out = int(usage.get("completion_tokens", 0))
        except Exception:
            return 0.0
        return (tokens_in * PRICE_IN_PER_M + tokens_out * PRICE_OUT_PER_M) / 1e6

    def refused(self):
        with self.lock:
            return self.calls >= CALL_CAP or self.usd >= USD_CAP

    def account(self, response_text):
        with self.lock:
            self.calls += 1
            self.usd += self.usage_usd(response_text)



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
    budget = Budget()

    def log_message(self, *_args):
        return

    def _forward(self, method):
        if self.path.rstrip("/").endswith("/chat/completions") and \
                self.budget.refused():
            # The budget caps are the proxy's own enforcement: refuse the
            # call WITHOUT forwarding or recording a completion.
            payload = json.dumps({"error": "budget exhausted",
                                  "calls": self.budget.calls,
                                  "usd": round(self.budget.usd, 6)})
            encoded = payload.encode("utf-8")
            self.send_response(402)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)
            return
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
        if self.path.rstrip("/").endswith("/chat/completions") and status == 200:
            Proxy.budget.account(payload)
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
    Proxy.budget.load_history(args.record)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Proxy)
    print(server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
