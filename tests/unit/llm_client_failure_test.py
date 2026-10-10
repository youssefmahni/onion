#!/usr/bin/env python3
"""Regression coverage for a thousand transport failures through curl multi."""
import argparse
import json
import subprocess
import sys
import tempfile
import socket
import struct
import threading
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"


def assert_transport(output):
    assert "requests_started=1000 requests_refused_budget=0 failed=1000" in output, output
    assert "transport=1000" in output, output
    assert "breaker_open=true" in output, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("closed-port", "reset", "timeout", "auth", "balance"), required=True)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if args.mode == "closed-port":
        # Port 9 is sometimes silently filtered on hosted Windows runners.
        # Reserve a loopback port then release it, guaranteeing no listener.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        completed = subprocess.run([str(args.binary), "llm", "bench", "--url", f"http://127.0.0.1:{port}", "--requests", "1000", "--limit", "100", "--timeout", "2"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        assert completed.returncode == 1, completed.stderr
        assert_transport(completed.stdout)
        return
    if args.mode in ("reset", "timeout"):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0)); listener.listen(2048)
        port = listener.getsockname()[1]
        stop = threading.Event()
        held = []
        def accept():
            while not stop.is_set():
                try:
                    listener.settimeout(.1); connection, _ = listener.accept()
                    if args.mode == "reset":
                        connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                              struct.pack("ii", 1, 0))
                        connection.close()
                    else:
                        # Keep each accepted stream open without creating a
                        # thread per socket.  That lets the test accept all
                        # 1,000 connections promptly even on macOS CI.
                        held.append(connection)
                except socket.timeout: pass
        thread = threading.Thread(target=accept, daemon=True); thread.start()
        try:
            timeout = "2" if args.mode == "reset" else "1"
            completed = subprocess.run([str(args.binary), "llm", "bench", "--url", f"http://127.0.0.1:{port}", "--requests", "1000", "--limit", "100", "--timeout", timeout], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
            assert completed.returncode == 1, completed.stderr
            if args.mode == "reset":
                assert_transport(completed.stdout)
            else:
                assert "timeouts_uncertain_billed=1000" in completed.stdout, completed.stdout
                assert "breaker_open=true" in completed.stdout, completed.stdout
        finally:
            stop.set()
            for connection in held: connection.close()
            listener.close()
        return
    with tempfile.TemporaryDirectory() as temporary:
        if args.mode in ("auth", "balance"):
            status = 401 if args.mode == "auth" else 402
            scenario = Path(temporary) / f"{args.mode}.json"
            scenario.write_text(json.dumps({"errors": {"status": status}}), encoding="utf-8")
            record = Path(temporary) / "record.jsonl"
            server = subprocess.Popen([sys.executable, str(SERVER), "--scenario", str(scenario), "--record", str(record), "--port", "0"], stdout=subprocess.PIPE, text=True)
            try:
                port = server.stdout.readline().strip()
                completed = subprocess.run([str(args.binary), "llm", "bench", "--url", f"http://127.0.0.1:{port}", "--requests", "3"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
                assert completed.returncode == 1, completed.stderr
                assert "requests_started=3 requests_refused_budget=0 failed=3" in completed.stdout, completed.stdout
                assert "breaker_open=true" in completed.stdout, completed.stdout
                assert f"breaker_reason={'authentication' if status == 401 else 'balance'}" in completed.stdout, completed.stdout
            finally:
                server.terminate(); server.wait(timeout=10)
            return


if __name__ == "__main__":
    main()
