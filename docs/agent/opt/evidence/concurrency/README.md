# Concurrency evidence — real DeepSeek (owner's measurements)

Raw CSVs from the owner's probe runs (one API key, model `deepseek-flash`,
`reasoning_effort: none`, real-size answers of about 365 output tokens).
Rows from 600 up used a shared DNS cache and a ramp of new connections.

| File | Levels covered |
|---|---|
| `rerun.csv` | 100, 200, 400 |
| `rerun2.csv` | 600, 700, 800 |
| `limit.csv` | 1,000, 1,400 |

## Summary (OPTIMIZATION_GOAL.md §4.2)

| In flight | Answers/s | p50 | p95 | p99 | Errors |
|---|---|---|---|---|---|
| 100 | 32.8 | 2.72 s | 3.2 s | 3.3 s | 0 |
| 200 | 58.1 | 2.79 s | 3.5 s | 3.7 s | 0 |
| 400 | 98–112 | 2.7–2.9 s | 4.2–4.3 s | 5.2–5.9 s | 0 |
| 600 | 141 | 2.81 s | 3.2 s | 3.3 s | 0 |
| 700 | 160 | 2.84 s | 3.4 s | 4.0 s | 0 |
| 800 | 177 | 2.88 s | 3.5 s | 4.1 s | 0 |
| **1,000** | **204–207** | 2.65 s | 3.1 s | 3.2–3.3 s | 2 network |
| 1,400 | 144–146 (−30%) | 2.82–2.84 s | 4.8–5.0 s | 8.6–9.1 s | 0 |

Two runs of 1,000 and 1,400 agree within 2%; run-to-run variation at the same
level is up to about 30%.

Findings that drive the design (F1–F6):

- **F1.** The peak is about 1,000 in flight (about 205 answers/s, about
  77,000 output tokens/s). Stable operation is 700–800.
- **F2.** Past the peak, the median barely moves while throughput falls 30%
  and p99 nearly triples. A limiter that watches the median cannot detect
  saturation; answers per window and p95/p99 can.
- **F3.** The client can be the bottleneck: without a shared DNS cache,
  hundreds of simultaneous lookups failed and new connections failed or
  timed out at 600–700. A shared DNS cache, shared TLS sessions and a ramp
  removed every error.
- **F4.** Heavy repeated testing caused a temporary block, which recovered
  within the hour.
- **F5.** Budget is the binding constraint: 1,000 in flight is about 278
  million output tokens per hour.
- **F6.** Untested: whether the limit is per account or per machine (W6
  prepares the owner-run experiment).

The probe tool that produced these numbers is `tools/llm_probe/`.
