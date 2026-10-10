#!/usr/bin/env python3
"""Proves the standalone fake accepts 1,000 simultaneous completions."""
import concurrent.futures
import json
import subprocess
import sys
import tempfile
import threading
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"
SCENARIO = ROOT / "tools" / "fake_deepseek" / "scenarios" / "healthy.json"


def main():
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "requests.jsonl"
        server = subprocess.Popen(
            [sys.executable, str(SERVER), "--scenario", str(SCENARIO), "--record", str(record), "--port", "0"],
            stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().strip())
            gate = threading.Barrier(1000)
            def one(_):
                gate.wait(timeout=30)
                body = json.dumps({"messages": [{"role": "user", "content": "concurrency"}]}).encode()
                request = urllib.request.Request(f"http://127.0.0.1:{port}/chat/completions", data=body,
                    headers={"Content-Type": "application/json"}, method="POST")
                with urllib.request.urlopen(request, timeout=45) as response:
                    return response.status
            with concurrent.futures.ThreadPoolExecutor(max_workers=1000) as pool:
                statuses = list(pool.map(one, range(1000)))
            assert statuses == [200] * 1000
        finally:
            server.terminate()
            server.wait(timeout=10)


if __name__ == "__main__":
    main()
