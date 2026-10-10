#!/usr/bin/env python3
"""Reproduce the M2 benchmark set against the fake DeepSeek server.

Usage: run_benchmarks.py <data_depot_binary> <output_dir>

Runs the rebuild-plan gate rows and the §11 M2 evidence set, writing one
<name>.txt per run and the three adaptive-limiter trace CSVs.  No real
DeepSeek calls: everything runs against the local fake server.
"""
import json
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
FAKE = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"
SCENARIOS = ROOT / "tools" / "fake_deepseek" / "scenarios"


class FakeServer:
    def __init__(self, scenario):
        self.record = Path(scenario).with_name(f"record_{Path(scenario).stem}.jsonl")
        self.record.unlink(missing_ok=True)
        self.process = subprocess.Popen(
            [sys.executable, str(FAKE), "--scenario", str(scenario), "--record", str(self.record), "--port", "0"],
            stdout=subprocess.PIPE, text=True)
        self.port = int(self.process.stdout.readline().strip())

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=10)


def run(binary, out_dir, name, args, check=False):
    completed = subprocess.run([str(binary)] + args, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
    (out_dir / f"{name}.txt").write_text(completed.stdout, encoding="utf-8")
    print(f"[{name}] rc={completed.returncode}")
    print(completed.stdout)
    if check:
        assert completed.returncode == 0, completed.stderr
    return completed.stdout


def trace_csv(binary, out_dir, scenario):
    completed = subprocess.run(
        [str(binary), "llm", "limiter-trace", "--scenario", scenario, "--csv"],
        text=True, stdout=subprocess.PIPE, check=True)
    (out_dir / f"limiter_trace_{scenario}.csv").write_text(completed.stdout, encoding="utf-8")
    print(f"[trace {scenario}] written")


def hanging_listener():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(2048)
    stop = threading.Event()
    held = []

    def accept():
        while not stop.is_set():
            try:
                listener.settimeout(.1)
                connection, _ = listener.accept()
                held.append(connection)  # never respond
            except socket.timeout:
                pass

    thread = threading.Thread(target=accept, daemon=True)
    thread.start()
    return listener, stop, held, thread


def main():
    binary, out = sys.argv[1], Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    # Adaptive limiter traces (G4).
    for scenario in ("healthy", "rate_limited", "slow_under_load"):
        trace_csv(binary, out, scenario)

    # 1. Capacity at limit 1,000 against the healthy scenario.
    server = FakeServer(SCENARIOS / "healthy.json")
    try:
        run(binary, out, "capacity_1000", ["llm", "bench", "--url", f"http://127.0.0.1:{server.port}",
                                           "--requests", "1000", "--limit", "1000", "--budget", "100", "--timeout", "30"])
    finally:
        server.stop()

    # 2. Connection reuse at limit 100.
    server = FakeServer(SCENARIOS / "healthy.json")
    try:
        run(binary, out, "reuse_100", ["llm", "bench", "--url", f"http://127.0.0.1:{server.port}",
                                       "--requests", "1000", "--limit", "100", "--budget", "100", "--timeout", "30"])
    finally:
        server.stop()

    # 3. Half-budget run: USD 5 budget, 0.01 reservations -> 500 started, 500 refused.
    server = FakeServer(SCENARIOS / "healthy.json")
    try:
        run(binary, out, "half_budget", ["llm", "bench", "--url", f"http://127.0.0.1:{server.port}",
                                         "--requests", "1000", "--limit", "1000", "--budget", "5", "--timeout", "30"])
    finally:
        server.stop()

    # 4. Timeout outage: 1,000 requests against a listener that accepts and never answers.
    listener, stop, held, thread = hanging_listener()
    try:
        run(binary, out, "timeout_outage", ["llm", "bench", "--url", f"http://127.0.0.1:{listener.getsockname()[1]}",
                                            "--requests", "1000", "--limit", "100", "--budget", "100", "--timeout", "1"])
    finally:
        stop.set()
        for connection in held:
            connection.close()
        listener.close()
        thread.join(timeout=10)

    # 5. 401 and 402 breaker runs (auth/balance paths).
    for name, status in (("auth_401", 401), ("balance_402", 402)):
        scenario = Path("/tmp") / f"m2_{name}.json"
        scenario.write_text(json.dumps({"errors": {"status": status}}), encoding="utf-8")
        server = FakeServer(scenario)
        try:
            run(binary, out, name, ["llm", "bench", "--url", f"http://127.0.0.1:{server.port}",
                                    "--requests", "3", "--limit", "8", "--budget", "100", "--timeout", "30"])
        finally:
            server.stop()

    # 6. 429 storm: every request rate-limited; breaker must stay closed.
    server = FakeServer(SCENARIOS / "rate_limited.json")
    try:
        run(binary, out, "rate_429_storm", ["llm", "bench", "--url", f"http://127.0.0.1:{server.port}",
                                            "--requests", "100", "--limit", "32", "--budget", "100", "--timeout", "30"])
    finally:
        server.stop()

    print("all benchmark evidence written to", out)


if __name__ == "__main__":
    main()
