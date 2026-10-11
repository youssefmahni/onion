#!/usr/bin/env python3
"""Wiring test: the limiter follows a mid-run capacity shift (decision 0011,
plateau probe adoption).  On Windows the Python fake server's own throughput
(about 30 concurrent requests) caps below the shift's target, so the shift
cannot be observed there: the test skips (exit 77)."""
import argparse
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
    if sys.platform == "win32":
        print("capacity_shift skipped on Windows: the fake server's own "
              "throughput caps below the shift's plateau")
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
