#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"
SCENARIOS = ROOT / "tools" / "fake_deepseek" / "scenarios"


def request(url, method="GET", payload=None):
    headers = {"Authorization": "Bearer should-not-be-recorded"}
    body = None if payload is None else json.dumps(payload).encode()
    if body is not None:
        headers["Content-Type"] = "application/json"
    with urllib.request.urlopen(urllib.request.Request(url, data=body, headers=headers, method=method), timeout=5) as response:
        return response.status, json.loads(response.read())


def main():
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "requests.jsonl"
        process = subprocess.Popen([sys.executable, str(SERVER), "--scenario", str(SCENARIOS / "healthy.json"), "--record", str(record), "--port", "0"], stdout=subprocess.PIPE, text=True)
        try:
            port = int(process.stdout.readline().strip())
            status, models = request(f"http://127.0.0.1:{port}/models")
            assert status == 200 and models["data"][0]["id"] == "deepseek-flash"
            status, completion = request(f"http://127.0.0.1:{port}/chat/completions", "POST", {"messages": [{"role": "user", "content": "hello"}]})
            assert status == 200 and completion["choices"][0]["finish_reason"] == "stop"
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if record.exists() and record.stat().st_size > 0:
                    break
                time.sleep(0.01)
            assert record.exists() and record.stat().st_size > 0
            recorded = json.loads(record.read_text(encoding="utf-8").strip())
            assert "Authorization" not in recorded["headers"] and "authorization" not in recorded["headers"]
        finally:
            process.terminate(); process.wait(timeout=5)


if __name__ == "__main__":
    main()
