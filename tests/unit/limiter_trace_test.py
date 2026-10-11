#!/usr/bin/env python3
"""Contract tests for the limiter-v2 traces (decision 0011)."""
import subprocess
import sys


def trace(binary, scenario):
    completed = subprocess.run([binary, "llm", "limiter-trace", "--scenario", scenario],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               check=True)
    return completed.stdout


def main():
    binary = sys.argv[1]
    healthy = trace(binary, "healthy")
    # Growth multiplies by 1.25 while goodput rises: 16 -> 20 -> 25 -> 31.
    assert "window_s=0 used_limit=16" in healthy
    assert "window_s=30 used_limit=20" in healthy
    assert "window_s=60 used_limit=25" in healthy
    assert "window_s=60" in healthy and "next_limit=31" in healthy
    # The hourly probe caps at max(start, limit/4) and restores the limit.
    assert "window_s=3600 used_limit=16" in healthy
    assert "window_s=3600" in healthy and "next_limit=58" in healthy

    limited = trace(binary, "rate_limited")
    # A 429 halves the limit and holds one window.
    assert "window_s=60 used_limit=25 rate_limited=1" in limited
    assert "window_s=60" in limited and "next_limit=12" in limited
    assert "window_s=90 used_limit=12 rate_limited=0" in limited
    assert "window_s=90" in limited and "next_limit=12" in limited
    # Recovery resumes the ramp.
    assert "window_s=120" in limited and "next_limit=15" in limited

    slow = trace(binary, "slow_under_load")
    # Goodput that stops rising reverts the increase and holds at the plateau:
    # the limit returns to 16 and stays there while goodput stays flat.
    assert "window_s=30 used_limit=20" in slow
    assert "window_s=30" in slow and "next_limit=16" in slow
    assert "window_s=150 used_limit=16" in slow
    assert "window_s=3600" in slow and "next_limit=16" in slow


if __name__ == "__main__":
    main()
