# 0017 — Funnel defaults v2 (W5 variants)

Status: **Accepted in principle** (owner, 2026-10-13), with one
amendment: the synthesizer respects a **target of 8–20 children with a
hard maximum of 30 and no padding** (implemented as the default
configuration). The defaults switch ships in the accompanying PR;
the gate defaults are NOT part of this decision (see DR 0020).

## Context

The W5 funnel experiments (`FUNNEL_OPT.md`) ran the real funnel on the
seeded sample with one factor changed at a time (V1–V7, PR #33's
campaign-spec knobs), measured tokens/quality per variant and judged
quality blind-pairwise against the baseline children. The baseline's
measured behaviour (W5_BASELINE.md): kept {100,10,7,100} on the four
non-leaf paths, average critique score 0.12 (gate 0.7 unreachable), zero
cache hits, 33% review rate.

## Proposed defaults

| Parameter | Baseline | Proposed | Evidence (FUNNEL_OPT.md + W5.4) |
|---|---|---|---|
| planner aspects | 4–8 | **3–5** | F2: −13/−26% tokens, 4/4 judge wins |
| trim rounds | 400 → 300 | **single 150** | F1: −53%/−40% tokens, score 0.11→0.21, 3/3 wins |
| shared prompt prefix (cache) | off | **on** | F3: nonzero cache hit measured, 3/3 wins |
| light funnel from | L8 | **L6** | F6: −49%/−48% tokens, 4.2× kept, score 0.27 vs 0.17 (per-node; see the whole-tree caveat below) |
| worker candidates per aspect | 100 | 40 (accepted trade) | F5: −40%/−62% tokens, judge 1–2 (now 12/12 on W5.4) |
| synthesizer range | 3..100 | **target 8–20, hard max 30, no padding** | owner's amendment; W5.4 kept {100,8,10,5} shows the raw model pads to 100 without the target |
| output format | items objects | unchanged (compact optional) | F5 |

Everything ships as campaign-spec fields (PR #33); accepting this
decision means making these the defaults the engine uses when the
campaign file omits them. One-line config change; nothing else moves.

## W5.4 validation (fresh stratified sample of 12, gate 0.35/0.15)

Reruns of the baseline, V3 and V1 on a fresh 12-path sample (4 TD + 4
L5 + 4 L6, seed 20261013, `~/w54-*` data dirs, 322 funnel calls + 36
judge calls — inside the approved 400):

| Run | attempts | in tok | out tok | kept | atomic | per-attempt in/out |
|---|---|---|---|---|---|---|
| baseline | 6 | 12,502 | 32,145 | 8 (+100 on the shared path) | 0 (+57) | 2,084 / 5,358 |
| V3 (trim 150) | 2 | 6,285 | 10,086 | 10 | 5 | 3,143 / 5,043 |
| V1 (40 candidates) | 1 | 5,068 | 5,603 | 5 | 5 | 5,068 / 5,603 |

- **Gate pass rate with the recalibrated pair:** every non-leaf path
  committed in-engine (1 per run on this leaf-heavy sample; score 0.32–
  0.35 vs the 0% at the old 0.7 gate). the recalibrated-pair measurement (DR 0020) works as
  predicted.
- **Growth factor** (children_kept × (1 − atomic_share), W5.4 sample):
  baseline 100/8, V3 10, V1 5. On the shared path the baseline kept 100
  children (57 labelled atomic — the model's pad-to-100 and
  over-labelling, both unchanged knobs; verification is off pending
  DR 0021's economics note), V3 kept 10, V1 kept 5.
- **Whole-tree projected cost** (from FUNNEL_OPT's re-ranking): V1
  $11.1B and V3 $4.02B at three levels vs the baseline's saturated
  ≥$26.9T. V4 and V7-light were re-ranked out on whole-tree cost.
- **Blind pairwise judgments (12 per comparison, deepseek-chat, fixed
  prompt, randomized order, Wilson 95% CIs):** baseline vs V3: **12/0**
  (CI 0.76–1.00) · baseline vs V1: **12/0** (CI 0.76–1.00) · V3 vs V1:
  **8/4 to V1** (V1 win rate CI 0.39–0.86). Both candidates beat the
  baseline decisively on the shared path; V1 leads V3 head-to-head.

## Consequences

- Per-node tokens fall 20–60% under the candidates; the growth factor
  on the W5.4 sample is 5–10 vs the baseline's 54–100, which is the
  decisive whole-tree lever (FUNNEL_OPT re-ranking).
- Risk: the fresh sample produced a single committable path per run
  (the sample is leaf-heavy), so the judge evidence is one path × 12
  judgments; the W5-era pair evidence (F1–F7) supplements it.

## Decision needed

Accept (make proposed defaults effective), reject, or accept a subset —
each knobs independently behind the campaign spec. DR 0020 (critic rubric)
is the companion decision.

## Projection for the exact recommended configuration

Growth factor `children_kept × (1 − atomic_share)` with the target's
midpoint (14) and atomic 0 (unverified labels never make leaves, DR
0021): **14** (vs the W5-measured baseline 54.25). Projection inputs
are synthetic parameters documented in the data dir
(`~/w54/combined`: 4 settles kept 14/atomic 0/attempts 2, 6 attempts of
3,143 in / 5,043 out — the V3 per-attempt profile); the commands are
the standard `campaign plan` invocations.

| Horizon | new nodes | cost peak / off-peak | time at 60k tokens/s |
|---|---|---|---|
| one more level | 4,427,696 | $3,356.11 / $1,678.06 | 11.15 hours |
| three levels deep | 3.61 × 10^12 | $2.74B / $1.37B | 9.10 × 10^6 hours |

Against the W5-measured baseline (growth 54.25): one level −51%
($6,786.58 → $3,356.11); three levels $2.74B vs the baseline's
saturated ≥$26.9T. The target range is the single largest whole-tree
lever.
