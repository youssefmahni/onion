#!/usr/bin/env python3
"""O2: the client's thread count is independent of the in-flight count
(100 vs 1,000 in flight).  Linux only: thread counts come from
/proc/<pid>/status; elsewhere the test skips (exit 77)."""
import subprocess
import sys
import time
from pathlib import Path

from fake_server_harness import fake_server


def thread_count(pid):
    status = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    for line in status.splitlines():
        if line.startswith("Threads:"):
            return int(line.split()[1])
    raise RuntimeError(f"no Threads line for pid {pid}")


def run_at_limit(binary, port, limit):
    """Runs the fixed bench at the given limit and returns the maximum
    observed thread count."""
    process = subprocess.Popen(
        [str(binary), "llm", "bench", "--url", f"http://127.0.0.1:{port}",
         "--requests", "3000", "--limit", str(limit), "--budget", "100",
         "--timeout", "30"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    maximum = 0
    try:
        deadline = time.monotonic() + 120
        while process.poll() is None and time.monotonic() < deadline:
            try:
                maximum = max(maximum, thread_count(process.pid))
            except (FileNotFoundError, RuntimeError):
                pass
            time.sleep(0.05)
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=10)
    return maximum, process.returncode


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if not Path("/proc/self/status").exists():
        print("skipped: /proc not available on this platform")
        sys.exit(77)
    with fake_server("healthy.json") as url:
        port = int(url.rsplit(":", 1)[1])
        low, low_rc = run_at_limit(args.binary, port, 100)
        high, high_rc = run_at_limit(args.binary, port, 1000)
        assert low_rc == 0, f"low-limit run failed: rc={low_rc}"
        assert high_rc == 0, f"high-limit run failed: rc={high_rc}"
        # The thread count must be independent of the in-flight count:
        # a small allowance covers measurement jitter only.
        assert high <= low + 5, f"threads grew with in-flight: {low} -> {high}"
        assert high <= 40, f"unexpected thread count: {high}"
        print(f"threads-ok: {low} at 100 in flight, {high} at 1,000")


if __name__ == "__main__":
    main()
