# GATE_CALIBRATION — why nothing passes the gate, and the atomic share

Date: 2026-10-13 · OPT track · zero new spend (cassette analysis).

## 1. The gate never fires: the critic is harsher than the legacy critic

**What the parser sees.** All 88 critique replies across the baseline and
the variants parse cleanly: every reply is strict JSON with `"score"` in
the 0–1 scale the prompt asks for (`{"score": 0.15, "remove": [...],
"add": [...], "issues": "..."}`). Distribution: min 0.08, max 0.62,
mean 0.306; quantized modes at 0.15, 0.35, 0.55, 0.62. No 0–10, 0–100
or percent forms appear — the parser was faithful.

**What the prompt asks.** `Critique this candidate set for the node.
This is a RAW CANDIDATE POOL meant to be filtered: score it on how many
candidates are usable direct children ... (precision), not on whether
every candidate is perfect.` — the same raw-pool-precision semantics as
the legacy critic's records (`~/onion-opt-data/funnel_critique.tsv`:
"the raw pool requires heavy filtering to be usable", "Massive
over-generation of near-duplicate ...").

**The legacy critic** (37 recorded rows, same prompt semantics): min
0.00, p25 0.18, median 0.42, p75 0.72, max 1.00; 29.7% scored ≥ 0.7.
The current critic (deepseek-flash) tops out at 0.62: the gate's
`pass_score 0.7 / floor 0.5` (DESIGN › Quality gate table) is above the
model's ceiling. Consequence measured on every run: **0 attempts at
≥0.7; 0–2 attempts ≥0.5**; every non-leaf node went to review (33%
review rate at baseline). The threshold pair was calibrated for the
legacy critic, not this model.

**Is it a parsing or prompt bug?** Neither — the parser and the prompt
scale agree with the model's output. The critic is genuinely harsher
with this model. (One parser gap found and fixed defensively anyway:
`parse_critique` accepted any number, so a future 0–10/0–100 drift
would silently mis-score the gate — it now rejects scores outside
[0, 1] with a test built from the recorded reply shapes plus drift
cases.)

**Recalibration from the blind-judge winners.** The 17 judged pairs'
winners scored: min 0.15, median 0.35, max 0.62 (losers: median 0.35 —
the score is a weak judge-signal, as expected: it scores the raw pool,
the judge scores the kept set). The winner distribution therefore
supports **pass 0.35 / floor 0.15** as the recalibrated pair: the
median winner commits, nothing below the worst winner auto-commits.
Would-be pass rates on the recorded attempts at pass 0.35: 8–33% per
run (vs 0% today) — proposed in DR 0018.

## 2. Atomic share 0: labels exist, the plumbing dropped them

- **The synthesizer labels atomics.** 85 recorded synthesizer replies
  carry 2,002 `"atomic": true` and 919 `"atomic": false` — proper
  booleans, parsed correctly. The model over-labels: ~69% of children
  are labelled atomic (the legacy funnel flagged only 30 in total).
- **The verification call never runs.** `atomic_verification_prompt`
  exists in `src/gate/gate.cpp` but has no call site (dead code since
  M3). DESIGN › Quality gate: a child labelled atomic is a leaf only
  after the separate `admits_children` check agrees; otherwise it stays
  a task. It now runs per labelled child behind `verify_atomic` (off by
  default in M3: ATOMIC has no store effect yet, and at a 69% label
  rate the per-child sweep is ~0.7×kept extra calls — cost noted in
  DR 0018).
- **Two bugs hid the labels:** (a) the needs-review branch stored the
  chosen children without their atomic flags — fixed: it now emits
  ATOMIC records like the Commit branch and carries the count in the
  NEEDS_REVIEW payload; (b) `review accept` settled with a hard-coded
  `atomic: 0` — fixed: it reads the count from the review record.
- **The labels did flow on commits.** The v3/v5/v6 runs committed
  nodes the floor allowed (0.55/0.62 ≥ 0.5): their settles recorded
  atomic 18/18, 94/94, 10/10 — 100% of kept children labelled atomic.
  With the gate recalibrated (more commits), the share the model
  produces is therefore ~1.0 pre-verification and (per DESIGN) small
  post-verification.

## Evidence anchors

- `/home/youssef/w5-*/**/cassette.jsonl` — 88 critique replies, 85
  synthesizer replies (request/response bodies).
- `/home/youssef/onion-opt-data/funnel_critique.tsv` — 37 legacy critic
  rows with scores and texts.
- `*/onion.db` `critiques`, `attempts`, `settles`, `reviews` tables.
- New tests: `GateParsers.CritiqueScoreScaleDriftIsRejected`,
  `GateFunnel.AtomicVerification*` (3), store `reviews.atomic` migration.

## 3. Do the scores predict the judge? (2026-10-13, no new spend)

All 20 unique judged pairs (17 W5-era pairs + the 3 W5.4 comparisons;
the W5.4 pairs were judged 12× each with different randomization, one
decision per comparison): winner's critique score vs loser's —
**strict agreement 8/20 (40%), disagreement 4/20, equal-score ties
8/20** (the score's 0.15/0.35 quantization makes 40% of pairs
inseparable). **AUC 0.60.** The raw-pool precision score is a weak
judge signal. Conclusion: calibrate the critic, not the threshold —
see DR 0020 (anchored rubric).
