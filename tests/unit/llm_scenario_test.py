#!/usr/bin/env python3
"""Wiring tests: the adaptive limiter against the fake-server scenarios of
decision 0011 (plateau_flat_median, capacity_shift, connect_fail_burst).

The absolute bands are tuned to the hosted Linux runner; on other machines
the fake server's own throughput shifts the equilibrium slightly, so the
assertions combine generous absolute bands with relative properties (the p95
spike, the step-back, the halving) that hold everywhere."""
import argparse
import re
from pathlib import Path

from fake_server_harness import run_adaptive


def windows(output):
    result = []
    for line in output.splitlines():
        if line.startswith("window="):
            fields = dict(re.findall(r"(\w[\w_]*)=([\w.+-]+)", line))
            result.append(fields)
    return result


def field(window, name):
    return float(window[name])


def limits(seen):
    return [int(field(w, "limit_after")) for w in seen]


def plateau_settles_and_tail_guard_fires(binary):
    """Against plateau_flat_median the limit settles in a band around the
    plateau, never exceeds the ceiling, and the median stays flat while the
    limit self-corrects (the p95-spike step-back itself is asserted by
    tail_guard_steps_back, whose tail threshold fires on every machine)."""
    out = run_adaptive(binary, "plateau_flat_median.json", 2500, 4, 200)
    seen = windows(out)
    assert len(seen) >= 12, f"expected a multi-window run, got {len(seen)}"
    for window in seen:
        assert field(window, "limit_used") <= 200, out
        assert field(window, "limit_after") <= 200, out
    first_p50 = field(seen[0], "p50_s")
    assert first_p50 > 0, "first window had no completed requests"
    for window in seen:
        # The median stays flat (F2) even under load.
        assert field(window, "p50_s") <= first_p50 * 1.6, f"median rose: {window}"
    # The run never climbs far past the plateau's band (the fake server's own
    # throughput is the constraint; the scenario's nominal N is 40).
    assert max(limits(seen)) <= 48, out
    # The last windows settle in a band around the plateau.
    final = limits(seen)[-5:]
    assert all(10 <= limit <= 48 for limit in final), final


def tail_guard_steps_back(binary):
    """The F2 saturation signature: a p95 spike steps the limit back in the
    same window.  The scenario's tail threshold (1 request) makes the spike
    fire on every machine; the bound is absolute because the scenario's
    fixed latency (50 ms) plus the 200 ms tail is machine-independent."""
    out = run_adaptive(binary, "tail_spike.json", 800, 4, 200)
    seen = windows(out)
    assert len(seen) >= 8, f"expected a multi-window run, got {len(seen)}"
    spike = None
    for window in seen:
        if field(window, "p95_s") >= 0.15:
            spike = window
            break
    assert spike is not None, "expected a p95 spike (the tail never fired)"
    assert field(spike, "limit_after") < field(spike, "limit_used"), spike
    # The guard keeps the limit near the threshold afterwards.
    assert limits(seen)[-1] <= 16, seen[-1]


def capacity_shift_recovers(binary):
    """When the provider's plateau doubles mid-run the limit follows it up
    (the plateau probe adopts the growth).  The same scenario machinery caps
    the plateau run near 33; here the equilibrium must climb past 45."""
    out = run_adaptive(binary, "capacity_shift.json", 5000, 4, 200)
    seen = windows(out)
    all_limits = limits(seen)
    for limit in all_limits:
        assert limit <= 200, out
    assert max(all_limits) >= 45, f"limit never recovered past the old plateau: {all_limits}"
    assert all_limits[-1] >= 30, all_limits


def connect_fail_burst_halves(binary):
    """A burst of connection failures halves the limit within one window."""
    out = run_adaptive(binary, "connect_fail_burst.json", 300, 4, 200)
    seen = windows(out)
    for window in seen:
        assert field(window, "limit_used") <= 200, out
    total_transport = sum(int(field(w, "transport")) for w in seen)
    assert total_transport >= 60, f"burst did not materialize: {total_transport}"
    burst = max(seen, key=lambda w: field(w, "transport"))
    assert field(burst, "transport") > 0, "no window with transport failures"
    # Backoff within one window: the limit after the burst window is at most
    # half the limit used in it.
    assert field(burst, "limit_after") <= field(burst, "limit_used") / 2.0, burst


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    plateau_settles_and_tail_guard_fires(args.binary)
    tail_guard_steps_back(args.binary)
    capacity_shift_recovers(args.binary)
    connect_fail_burst_halves(args.binary)
    print("scenarios-ok")


if __name__ == "__main__":
    main()
