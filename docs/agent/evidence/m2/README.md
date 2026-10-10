# M2 evidence — adaptive limiter traces and benchmark set

Reproduced with `python3 run_benchmarks.py <data_depot> .` against
`tools/fake_deepseek` (Python stdlib, HTTP/1.1) on Kali Linux, curl 8.22.0,
GCC 16.2. No real DeepSeek calls. `http_version=2` is curl's enum for
HTTP/1.1; HTTP/2 is `3` and is first exercised in the real-call validation.

## Adaptive limiter traces (G4, §11 M2.4)

Limiter start 16, ceiling 2,000, compressed 30-second windows
(`data_depot llm limiter-trace --scenario <s> --csv`). Columns:
`window_start_s,window_end_s,limit_used,probe,rate_limited,median_latency_s,limit_after`.

| Requirement | Where it is shown |
| --- | --- |
| Growth while healthy | `limiter_trace_healthy.csv`: limit 16→20→24→28→32→36→40 at median 0.1 s |
| Halving plus a one-window hold on 429 | `limiter_trace_rate_limited.csv`: 24→12 at window 60 (`rate_limited=1`), held at 12 through window 90, then growth |
| Holding while latency rises | `limiter_trace_slow_under_load.csv`: median 0.2 s > 1.5× baseline keeps the limit at 20 through windows 30–180 |
| Recovery | `limiter_trace_rate_limited.csv`: 12→16→20 once the hold ends |
| Hourly probe capping then restoring the limit | `probe=1` row at window 3600 in each CSV: the window runs at 16 (`max(starting, limit/4)`) while the pre-probe limit is restored afterwards (`healthy`: 16 used, back to 40; `slow`: 16 used, back to 20) |

## Benchmark set (§11 M2.5)

| Run | Result |
| --- | --- |
| `capacity_1000.txt` | 1,000 started at limit 1,000; 703.00 req/s; p50 0.831 s, p95 1.345 s, p99 1.386 s; peak in-flight 1,000; **14 process threads**; spend 1.00 ≤ budget 100 |
| `reuse_100.txt` | 1,000 requests at limit 100; **100 connections opened**; peak active 100; 14 process threads |
| `half_budget.txt` | USD 5 budget: **500 started, 500 refused**; final spend 0.50; no overspend (G9: kept, waiting-ticket test lands in M3) |
| `timeout_outage.txt` | 1,000 requests, 1 s deadline: 1,000 Timeouts, `timeouts_uncertain_billed=1000`, full reservations retained (spend 10.00), breaker open |
| `auth_401.txt` / `balance_402.txt` | 3 requests each: breaker opens with reason `authentication` / `balance` (no retries) |
| `rate_429_storm.txt` | 100 requests, 100% 429 with Retry-After 2: all rate-limited, backoff honoured, **breaker stays closed** (429 never counts) |

## Regeneration

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug
python3 docs/agent/evidence/m2/run_benchmarks.py build/linux-debug/data_depot docs/agent/evidence/m2
```

Expected wall time: ~2 minutes.


## Real DeepSeek validation and probe (§11 M2.6–2.7)

`real_probe.txt` holds the 100-call concurrency probe (adaptive limiter, start
4, ceiling 50, `max_tokens` 16): 100/100 OK, no 429/503, HTTP/2 with h2
multiplexing (4 connections), breaker closed, limit growing +4 per window at
~1.0 s latency. The 5-call request-shape validation (models list, usage field
names, `finish_reason` handling, 401 breaker path) is recorded in decision
0004's 2026-10-10 update and in PROGRESS.md › Real API usage.
