#!/usr/bin/env python3
"""Contract tests for the compressed 30-second adaptive limiter traces."""
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
    assert "window_s=0 used_limit=16" in healthy
    assert "window_s=30 used_limit=20" in healthy
    assert "next_limit=24" in healthy

    limited = trace(binary, "rate_limited")
    assert "window_s=60 used_limit=24 rate_limited=1" in limited
    assert "window_s=90 used_limit=12 rate_limited=0 median_latency_s=0.1 next_limit=12" in limited
    assert "window_s=120 used_limit=12 rate_limited=0 median_latency_s=0.1 next_limit=16" in limited

    slow = trace(binary, "slow_under_load")
    assert "window_s=30 used_limit=20 rate_limited=0 median_latency_s=0.2 next_limit=20" in slow
    assert "window_s=60 used_limit=20 rate_limited=0 median_latency_s=0.2 next_limit=20" in slow
    assert "window_s=3600 used_limit=16 rate_limited=0 median_latency_s=0.1 next_limit=20" in slow
    assert "window_s=3630 used_limit=20 rate_limited=0 median_latency_s=0.1 next_limit=24" in slow


if __name__ == "__main__":
    main()
