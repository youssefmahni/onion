#!/usr/bin/env python3
"""Wiring tests: the adaptive limiter against the fake-server scenarios of
decision 0011 (plateau_flat_median, capacity_shift, connect_fail_burst)."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"
SCENARIOS = ROOT / "tools" / "fake_deepseek" / "scenarios"


def run_adaptive(binary, scenario, requests, start, ceiling, window_s=1):
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "record.jsonl"
        server = subprocess.Popen(
            [sys.executable, str(SERVER), "--scenario", str(SCENARIOS / scenario),
             "--record", str(record), "--port", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        try:
            port = int(server.stdout.readline().strip())
            completed = subprocess.run(
                [str(binary), "llm", "bench", "--adaptive",
                 "--url", f"http://127.0.0.1:{port}", "--window-s", str(window_s),
                 "--requests", str(requests), "--start", str(start),
                 "--ceiling", str(ceiling), "--timeout", "20"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=240)
            return completed.stdout
        finally:
            server.terminate()
            server.wait(timeout=10)


def windows(output):
    result = []
    for line in output.splitlines():
        if line.startswith("window="):
            fields = dict(re.findall(r"(\w[\w_]*)=([\w.+-]+)", line))
            result.append(fields)
    return result


def field(window, name):
    return float(window[name])


def plateau_settles_and_tail_guard_fires(binary):
    """Against plateau_flat_median the limit settles in a band around the
    plateau, never exceeds the ceiling, the median stays flat, and a p95
    spike (the F2 saturation signature) steps the limit back."""
    out = run_adaptive(binary, "plateau_flat_median.json", 2500, 4, 200)
    seen = windows(out)
    assert len(seen) >= 12, f"expected a multi-window run, got {len(seen)}"
    for window in seen:
        assert field(window, "limit_used") <= 200, out
        assert field(window, "limit_after") <= 200, out
    first_p50 = field(seen[0], "p50_s")
    first_p95 = field(seen[0], "p95_s")
    assert first_p50 > 0
    spike = None
    for window in seen:
        # The median stays flat (F2) even while the tail rises.
        assert field(window, "p50_s") <= first_p50 * 1.6, f"median rose: {window}"
        if field(window, "p95_s") >= first_p95 * 2.0:
            spike = window
    assert spike is not None, "expected a p95 spike past the plateau"
    # The tail guard steps the limit back in the spike window.
    assert field(spike, "limit_after") < field(spike, "limit_used"), spike
    # The run never climbs past the plateau's band (the fake server's own
    # throughput is the constraint; the scenario's nominal N is 40).
    assert max(int(field(w, "limit_after")) for w in seen) <= 40, out
    # The last windows settle in a band around the plateau.
    final = [int(field(w, "limit_after")) for w in seen[-5:]]
    assert all(16 <= limit <= 40 for limit in final), final


def capacity_shift_recovers(binary):
    """When the provider's plateau doubles mid-run the limit follows it up
    (the plateau probe adopts the growth).  The same scenario machinery caps
    the plateau run at 33; here the equilibrium must climb past 50."""
    out = run_adaptive(binary, "capacity_shift.json", 5000, 4, 200)
    seen = windows(out)
    limits = [int(field(w, "limit_after")) for w in seen]
    for limit in limits:
        assert limit <= 200
    assert max(limits) >= 50, f"limit never recovered past the old plateau: {limits}"
    assert limits[-1] >= 40, limits


def connect_fail_burst_halves(binary):
    """A burst of connection failures halves the limit within one window and
    the limit recovers afterwards."""
    out = run_adaptive(binary, "connect_fail_burst.json", 300, 4, 200)
    seen = windows(out)
    for window in seen:
        assert field(window, "limit_used") <= 200
    burst = None
    for window in seen:
        if field(window, "transport") >= 50:
            burst = window
            break
    assert burst is not None, "expected a window with a transport burst"
    # Backoff within one window: the limit after the burst window is at most
    # half the limit used in it.
    assert field(burst, "limit_after") <= field(burst, "limit_used") / 2.0, burst
    # The run recovers above the floor afterwards.
    assert int(field(seen[-1], "limit_after")) >= 2, seen[-1]


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    plateau_settles_and_tail_guard_fires(args.binary)
    capacity_shift_recovers(args.binary)
    connect_fail_burst_halves(args.binary)
    print("scenarios-ok")


if __name__ == "__main__":
    main()
