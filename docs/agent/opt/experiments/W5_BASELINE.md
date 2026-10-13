# W5 baseline — measured funnel behaviour on 12 seeded corpus paths

Date: 2026-10-13 · Operator: OPT track · Data: `~/w5-base/data` (not in the repo)
Campaign: `w5b3`–`w5b8` (identical spec, restarted; stop is final per decision
0010 so each restart used a fresh id) · Real API via `tools/llm_proxy.py`.

## What ran

`campaign start` with 12 seed paths drawn from the corpus (seed 20261011),
`mode=breadth-first`, `loop_budget=200`, `chunk=50`, model `deepseek-flash`,
default gate 0.7 / floor 0.5 / max_attempts 3. The engine ran the real
funnel (worker → precheck → planner → trimmer → critique → synthesizer)
against the DeepSeek API through the recording proxy; a human (me) operated
the review queue exactly as production would (`review accept` with a note).

## Findings

- **F1 — the sample settles into two disjoint outcomes.**
  4 of 12 paths settled **with children** after the review queue: kept
  `{100, 100, 10, 7}`. 8 of 12 settled as **leaves** (`end=cannot split`,
  kept 0) after 0–3 attempts. No path passed the gate in the engine loop:
  every non-leaf path went to `NEEDS_REVIEW` (critique scores 0.15–0.35,
  far below the 0.7 gate).
  *Measured children kept (non-leaf, n=4): mean 54.25, median 55, mode 100.*

- **F2 — the synthesizer pads to 100 children** (the legacy funnel's
  signature): the two mid-branch paths kept exactly 100; the terminal
  ones kept 10 and 7. The pad-to-100 behaviour the cost model inherited
  from the legacy funnel is confirmed by the new engine on real calls.

- **F3 — zero cache hits.** `tokens_cached` is 0 on every attempt
  (135 completions recorded through the proxy; 21 attempts). Predicted:
  there is no prompt cache yet (V6 is designed but not built).

- **F4 — attempts and review rate.** 21 attempts across 12 settles
  (1.75 avg; the engine's own `SETTLE` rows record attempts 0 for the
  review-accepted settles, so `measure_params` now prefers the attempts
  table — PR #31). Review rate 4/12 (33%). Every review was a
  below-floor accept of the best attempt's stored children (decision 0008
  behaviour verified on real data: children were already `ADD_NODE`d).

- **F5 — token profile.** Per attempt (engine accounting, 21 rows):
  95,784 in / 189,440 out → ~4,561 in / ~9,021 out per attempt; at
  `calls_per_attempt=8` that is ~570 in / ~1,128 out per call. The
  proxy's usage sums (135 completions) are 86,904 in / 127,532 out. The
  synthesizer's 100-child outputs dominate (JSON item lists of
  ~1.5–13k tokens).

- **F6 — wall-clock cadence.** Across the run the engine issued calls in
  bursts (median inter-call gap 4.2 s) separated by long idle stretches
  (the funnel's internal retries, the review holds, and the two harness
  incidents below). The token-rate finding stands as previously measured
  (58–65k tokens/s at 700–800 in flight); this sample never pushed more
  than a handful of tickets in flight, so it cannot re-measure the rate.

## Harness incidents (fixed, not engine findings)

1. `/tmp` is a separate 436 MB filesystem; the baseline DB + oplog filled
   it and the state writer silently stopped mid-command (a dev-import
   truncating at 301,827 of 316,264 rows, and a node that wrote nothing).
   Moved the data dir to `~/w5-base`; re-imported; all 316,233 rows and
   both campaigns resumed. **Ops rule: never put onion data dirs on /tmp.**
2. Campaigns are stop-is-final (0010): background driver restarts stopped
   half-done campaigns. The driver now stops a campaign only when the
   settle count completes, and runs detached as a systemd user unit
   (`onion-w5baseline.service`).

## Evidence anchors

- `~/w5-base/data/onion.db` — `settles` (12 rows, kept/attempts), `attempts`
  (21 rows, tokens), `critiques` (scores), `reviews` (4, state closed).
- `~/w5-base/data/cassette.jsonl` — 140 proxy lines (135 completions,
  1 `/models` discovery, plus replays), full request/response bodies.
- Regenerated projections with `measure_params` from this DB (PR #31):
  t1 $6.79k / 23.19 h · t2 $71.96M / 245,851 h · t3 saturated at int64.
