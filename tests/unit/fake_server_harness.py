#!/usr/bin/env python3
"""Shared harness: spawn the scriptable fake DeepSeek server on a free port."""
import contextlib
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"


@contextlib.contextmanager
def fake_server(scenario_name, scenario_dir=None):
    """Yields the server's base URL; terminates the server afterwards.  The
    server's stderr is drained to a list (surfaced in test failures), so a
    crash inside the fake never hangs the test or hides the traceback."""
    scenarios = scenario_dir or (ROOT / "tools" / "fake_deepseek" / "scenarios")
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "record.jsonl"
        server = subprocess.Popen(
            [sys.executable, str(SERVER), "--scenario", str(scenarios / scenario_name),
             "--record", str(record), "--port", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stderr_lines = []
        drainer = threading.Thread(
            target=lambda: [stderr_lines.append(line) for line in server.stderr],
            daemon=True)
        drainer.start()
        try:
            port = int(server.stdout.readline().strip())
            yield f"http://127.0.0.1:{port}", stderr_lines
        finally:
            server.terminate()
            server.wait(timeout=10)


def run_adaptive(binary, scenario_name, requests, start, ceiling, window_s=1):
    """Runs `llm bench --adaptive` against the scenario and returns stdout
    (with the server's stderr appended, for failure diagnosis)."""
    with fake_server(scenario_name) as (url, server_stderr):
        completed = subprocess.run(
            [str(binary), "llm", "bench", "--adaptive",
             "--url", url, "--window-s", str(window_s),
             "--requests", str(requests), "--start", str(start),
             "--ceiling", str(ceiling), "--timeout", "20"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=240)
        output = completed.stdout
        if server_stderr:
            output += "\nSERVER-STDERR:\n" + "".join(server_stderr)
        return output
