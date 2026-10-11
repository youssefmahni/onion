# OPT track — AUDIT (W1)

Status of the repository at the start of the OPT track, with evidence.
All paths refer to `origin/main` as of 2026-10-11 (`c074d9e`, "Merge pull
request #11 from youssefmahni/m5-profiles-service").

## 1. Milestone state

| Milestone | State | Evidence |
|---|---|---|
| M0 | merged, tag `m0` | `git tag -l` lists `m0` |
| M1 | merged, tag `m1` | PR #1, tag `m1` |
| M2 | merged, tag `m2-rc` | PR #4, tag `m2-rc` |
| M3 | merged, tag `m3-rc` | PR #7, tag `m3-rc` |
| M4 | merged, tag `m4-rc` | PR #8, tag `m4-rc` |
| M5 | merged, tag `m5-rc` | PR #11, tag `m5-rc` |
| M6 | **open** | PR #12 `m6-sync` (Agent M) |

Gate reports exist for M1–M6 in `docs/agent/gates/`. The modules that exist:
`src/llm` (client, limiter), `src/gate`, `src/store`, `src/state`, `src/log`,
`src/profile`, `src/sched`, `src/cli`, `src/api`, `src/sync` (stub), `src/swim`
(stub). There is no `tests/fuzz/` directory and no `opt-nightly.yml` workflow.

## 2. Corpus copy (`~/onion-opt-data`)

Copied once from `~/Desktop/PlanningProcess` with rsync on 2026-10-11.
Node counts per level, compared with OPTIMIZATION_GOAL.md §4.1:

| Level | Expected | Measured (rows minus header) |
|---|---|---|
| L0 | 9 | 9 |
| L1 | 17 | 17 |
| L2 | 95 | 95 |
| L3 | 818 | 818 |
| L4 | 15,779 | 15,779 |
| L5 | 91,047 | 91,047 |
| L6 | 208,499 | 208,499 |
| L7–L9 | 0 | 0 |
| **Total** | **316,264** | **316,264** |

All counts match. Two small discrepancies against §4.1, recorded for honesty:
`funnel_state.tsv` holds **11** settled rows (brief says "about 12") and
`atomic_items.tsv` holds **29** atomic flags (brief says "about 30"). Both are
within the brief's own hedge ("only 12 … only 30"); the copy is what the
measurements in W5 will use, and those files are the source of truth for the
sample.

## 3. Implementation status of the eight owner items (W1 step 4)

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Goodput-based limiter | **Missing** | `src/llm/limiter.cpp` controls on median latency only: grow `+4` when `median <= baseline × 1.5` (`limiter.cpp:50-54`), halve on 429/503/>5% timeouts (`limiter.cpp:40-44`). No output-tokens-per-second signal exists anywhere in `src/llm/`. |
| 2 | Multiplicative ramp-up | **Missing** | Growth is additive `+4` per healthy window (`limiter.cpp:53`), per the M2 rule (decision 0005, DESIGN.md › Concurrency). No ×1.25 growth rule. |
| 3 | p95 hold | **Missing** | `LimitWindow` tracks only `median_latency_s` (`src/llm/llm.hpp:20`); no p95 is computed or stored. |
| 4 | Shared DNS cache and connection pool | **Partial** | One curl multi handle per process shares the connection pool and multiplexing (`src/llm/client.cpp:272-275`, `CURLPIPE_MULTIPLEX`). No `CURLSH` share handle, so the DNS cache is per easy handle, not shared: each handle does its own lookups (F3 risk). |
| 5 | Ramp of new connections | **Missing** | Nothing caps new connections per second; tasks are added to the multi handle as fast as they arrive (`client.cpp:352-367`). `CURLMOPT_MAX_TOTAL_CONNECTIONS` is not set. |
| 6 | HTTP/2 multiplexing | **Present** | `CURLPIPE_MULTIPLEX` (`client.cpp:274`) + `CURL_HTTP_VERSION_2TLS` with HTTP/1.1 fallback (`client.cpp:323`). Windows is HTTP/1.1 only (accepted limitation, decision 0002). |
| 7 | Profile ceilings v2 | **Missing** | `rules_v1` keeps the old values: desktop ceiling 2,000 / start 16; laptop ceiling 32 / start 4 (`src/profile/profile.cpp:90-108`), `rules_version = 1` (`profile.hpp:84`). Nothing measures a laptop ceiling from a probe. |
| 8 | Spend per hour in the projection | **Missing** | `campaign start` prints `loop_budget × average attempt cost` only (`src/cli/commands.cpp:472-480`); no per-hour rate, no peak/off-peak split, no log-growth projection. |

Summary: 2 present/partial of 8; the OPT track's W2 (limiter v2 + client
hygiene) covers items 1–5, item 7 is W2's profile-rules-v2 decision record,
item 8 is W4's `campaign plan` work. Nothing above is a surprise relative to
the M2/M5 scope.

## 4. First traceability gaps (details for W3)

- **C1 gap — cost projection:** DESIGN.md › Campaigns says the projection uses
  "the 7-day average, or a 20-attempt calibration run for a new model" and
  `LLM provider › Metering` says "Cost projections show both peak and off-peak
  totals". The code uses the device's total spend ÷ attempts and no peak/off-peak
  split (`commands.cpp:472-480`). Covered by W4, not a W3 blocker.
- **C3 gap — fuzzing:** no `tests/fuzz/` targets exist for the LLM response
  parser, canonical JSON, batch/record parser, CSV/TSV import, or HTTP request
  parsing. W3 adds them.
- **C4 gap — crash consistency at every commit step:** a test-only crash seam
  exists (`tests/unit/state_test.cpp:219-230`, `options.crash_hook` and
  `run_writer_until_crash`), and the torn-tail test runs at every byte offset
  (`state_test.cpp:377`). What is missing is a named kill point per DESIGN.md
  commit-order step (attempt durable → terminal batch → durable write → store
  update → notify) plus the restart assertions (no committed work lost, no
  children before the gate decision, attempt number and spend preserved). W3
  extends the seam; coordination with M7 chaos tools noted in COORDINATION.md
  when W3 starts.
- **C2 gap — flake hunt and TSan:** not yet run on this track; W3.
- **C5 gap — real smoke:** M2's real-call validation is recorded in decision
  0004 (request shape, usage fields, truncation, 401). The OPT-track C5 re-run
  is cheap (≤ 10 calls) and is planned for W3.

## 5. Agent M areas not touched (read-only)

`src/sync/`, `src/swim/`, the lane/stealing parts of `src/sched/`,
`tests/fleet/`, `tools/chaos/`, `docs/agent/PROGRESS.md`, `docs/agent/WORKLOG.md`,
`docs/agent/gates/M6*.md`, `.github/workflows/ci.yml`. The open M6 convergence
flake (§4.4 of the goal) is read-only review material for W7.
