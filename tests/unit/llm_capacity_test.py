#!/usr/bin/env python3
"""Wiring test: the limiter follows a mid-run capacity shift (decision 0011,
plateau probe adoption).

NIGHTLY ONLY: the assertion depends on the fake server's throughput, which
depends on the runner's speed, not on product behaviour (AUDIT.md).  The
deterministic plateau-probe adoption coverage lives in
tests/unit/limiter_test.cpp and runs on every OS in ci.yml.  In the normal
CI matrix this test skips (exit 77)."""
import argparse
import os
import re
import sys
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if os.environ.get("ONION_NIGHTLY") != "1":
        print("skipped: timing-sensitive wiring test; runs in opt-nightly.yml")
        sys.exit(77)
    # The Python fake server's own throughput caps well below the shift's
    # target on the hosted macOS and Windows runners (about 28-30 concurrent
    # requests there), so the shift is only observable on Linux.
    if sys.platform != "linux":
        print("capacity_shift skipped on this platform: the fake server's "
              "own throughput caps below the shift's plateau")
        sys.exit(77)
    out = run_adaptive(args.binary, "capacity_shift.json", 5000, 4, 200)
    seen = windows(out)
    all_limits = limits(seen)
    for limit in all_limits:
        assert limit <= 200, out
    # The plateau scenario's equilibrium is ~33 on the same machinery; 40
    # shows the probe adopted the doubled capacity.
    assert max(all_limits) >= 40, f"limit never recovered past the old plateau: {all_limits}"
    assert all_limits[-1] >= 30, all_limits
    print("capacity-ok")


if __name__ == "__main__":
    main()
