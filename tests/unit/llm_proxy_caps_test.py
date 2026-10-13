#!/usr/bin/env python3
"""Tests for the llm_proxy budget caps (owner rule: the proxy enforces
the 4,000-call / $5.00 lifetime caps itself and refuses calls beyond).

Standard library only. The proxy is driven against a local fake
upstream; small caps are injected via ONION_PROXY_CALL_CAP /
ONION_PROXY_USD_CAP so the tests stay fast and deterministic.
"""

import json
import os
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PROXY = ROOT / "tools" / "llm_proxy.py"

FAKE_USAGE = {"prompt_tokens": 1000, "completion_tokens": 1000}  # 0.00137 USD

UPSTREAM_HITS = 0


class FakeUpstream(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        return

    def do_POST(self):
        global UPSTREAM_HITS
        UPSTREAM_HITS += 1
        payload = json.dumps({
            "choices": [{"message": {"content": "x"}}],
            "usage": FAKE_USAGE,
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def start_server():
    server = ThreadingHTTPServer(("127.0.0.1", 0), FakeUpstream)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def start_proxy(record, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    process = subprocess.Popen(
        [sys.executable, str(PROXY), "--port", "0", "--record", str(record)],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env)
    port = int(process.stdout.readline().strip())
    return process, port


def post(url):
    body = json.dumps({"model": "deepseek-flash", "messages": []}).encode()
    request = urllib.request.Request(url, data=body,
                                     headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def main():
    failures = []

    def check(name, condition):
        print(("PASS" if condition else "FAIL") + " " + name)
        if not condition:
            failures.append(name)

    global UPSTREAM_HITS

    # 1. The call cap refuses further completions without forwarding.
    UPSTREAM_HITS = 0
    upstream = start_server()
    record = Path("/tmp") / "llm_proxy_caps_test.jsonl"
    record.unlink(missing_ok=True)
    proxy, port = start_proxy(record, {
        "ONION_PROXY_CALL_CAP": "2", "ONION_PROXY_USD_CAP": "5.00",
        "DEEPSEEK_API_KEY": "test",
        "ONION_PROXY_UPSTREAM": f"http://127.0.0.1:{upstream.server_address[1]}"})
    time.sleep(0.5)
    url = f"http://127.0.0.1:{port}/v1/chat/completions"
    status1, _ = post(url)
    status2, _ = post(url)
    status3, body3 = post(url)
    check("first call under the call cap passes (200)", status1 == 200)
    check("second call at the cap passes (200)", status2 == 200)
    check("third call over the call cap is refused (402)", status3 == 402)
    check("refusal body names the budget", b"budget exhausted" in body3)
    check("only the two allowed calls reach the upstream",
          UPSTREAM_HITS == 2)
    proxy.terminate()
    upstream.shutdown()
    record.unlink(missing_ok=True)

    # 2. The USD cap refuses on the dollar total.
    upstream = start_server()
    record = Path("/tmp") / "llm_proxy_usd_test.jsonl"
    record.unlink(missing_ok=True)
    # Two completions at ~0.00137 USD exceed a 0.002 cap after two calls.
    proxy, port = start_proxy(record, {
        "ONION_PROXY_CALL_CAP": "4000", "ONION_PROXY_USD_CAP": "0.002",
        "DEEPSEEK_API_KEY": "test",
        "ONION_PROXY_UPSTREAM": f"http://127.0.0.1:{upstream.server_address[1]}"})
    time.sleep(0.5)
    url = f"http://127.0.0.1:{port}/v1/chat/completions"
    status1, _ = post(url)
    status2, _ = post(url)
    status3, _ = post(url)
    check("USD cap: first call passes", status1 == 200)
    check("USD cap: second call passes (still under 0.002)", status2 == 200)
    check("USD cap: third call refused", status3 == 402)
    proxy.terminate()
    upstream.shutdown()
    record.unlink(missing_ok=True)

    # 3. The history in the record file counts toward the lifetime cap.
    upstream = start_server()
    record = Path("/tmp") / "llm_proxy_hist_test.jsonl"
    record.unlink(missing_ok=True)
    with record.open("w") as out:
        out.write(json.dumps({
            "ts": time.time(), "method": "POST",
            "path": "/v1/chat/completions", "headers": {}, "body": "{}",
            "status": 200,
            "response": json.dumps({"choices": [], "usage": FAKE_USAGE}),
        }) + "\n")
    proxy, port = start_proxy(record, {
        "ONION_PROXY_CALL_CAP": "1", "ONION_PROXY_USD_CAP": "5.00",
        "DEEPSEEK_API_KEY": "test",
        "ONION_PROXY_UPSTREAM": f"http://127.0.0.1:{upstream.server_address[1]}"})
    time.sleep(0.5)
    url = f"http://127.0.0.1:{port}/v1/chat/completions"
    status1, _ = post(url)
    check("history counts: a fresh proxy refuses at the pre-spent cap",
          status1 == 402)
    proxy.terminate()
    upstream.shutdown()
    record.unlink(missing_ok=True)

    if failures:
        print(f"{len(failures)} FAILURES")
        return 1
    print("all proxy cap checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
