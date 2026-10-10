#!/usr/bin/env python3
"""Regression coverage for a thousand transport failures through curl multi."""
import argparse
import json
import subprocess
import sys
import tempfile
import socket
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"


def assert_transport(output):
    assert "requests=1000 failed=1000" in output, output
    assert "transport=1000" in output, output
    assert "breaker_open=true" in output, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("closed-port", "reset"), required=True)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if args.mode == "closed-port":
        # Port 9 is sometimes silently filtered on hosted Windows runners.
        # Reserve a loopback port then release it, guaranteeing no listener.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        completed = subprocess.run([str(args.binary), "llm", "bench", "--url", f"http://127.0.0.1:{port}", "--requests", "1000", "--timeout", "2"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        assert completed.returncode == 1, completed.stderr
        assert_transport(completed.stdout)
        return
    with tempfile.TemporaryDirectory() as temporary:
        scenario = Path(temporary) / "reset.json"
        scenario.write_text(json.dumps({"reset_connection_rate": 1.0}), encoding="utf-8")
        record = Path(temporary) / "record.jsonl"
        server = subprocess.Popen([sys.executable, str(SERVER), "--scenario", str(scenario), "--record", str(record), "--port", "0"], stdout=subprocess.PIPE, text=True)
        try:
            port = server.stdout.readline().strip()
            completed = subprocess.run([str(args.binary), "llm", "bench", "--url", f"http://127.0.0.1:{port}", "--requests", "1000", "--timeout", "2"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            assert completed.returncode == 1, completed.stderr
            assert_transport(completed.stdout)
        finally:
            server.terminate()
            server.wait(timeout=10)


if __name__ == "__main__":
    main()
