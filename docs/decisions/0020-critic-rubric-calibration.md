# 0020 — Critic rubric calibration (Proposed)

Status: **Proposed** (owner decision required). Claimed by the OPT track;
next free number at claim time. (The earlier OPT draft "gate
recalibration" carried 0018; Agent M took 0018/0019 on main, so this
record is 0020.)

## Context

`GATE_CALIBRATION.md`: the critic parses faithfully on the 0–1 scale it
was asked for (88/88 replies), but the deepseek-flash critic tops out at
0.62 (mean 0.31, quantized modes 0.15/0.35) against the 0.7/0.5 gate —
the pair inherited from the legacy critic (p75 0.72).

**Do the scores predict the blind judge?** All 20 unique judged pairs
(17 W5-era + 3 W5.4 comparisons), winner's critique score vs loser's:
strict agreement 8/20 (40%), disagreements 4/20, and 8/20 pairs where
the two scores are equal so no prediction is possible. AUC 0.60. The
raw-pool precision score is a weak judge signal — the score's 0.15/0.35
quantization cannot separate the pairs the judge prefers. A threshold
change alone would therefore calibrate noise, not quality.

## Proposal

1. **Anchored rubric (the fix).** The critique prompt gains a fixed
   rubric with example anchor sets at known scores — e.g., an
   obviously-usable pool scored 0.8, a mixed pool 0.5, a mostly-off-topic
   pool 0.2 — instructing the critic to score the candidate pool
   relative to these anchors. Anchors are fixed text (prompt-cache
   friendly, V6's prefix), no new calls.
2. **Keep the gate at 0.7/0.5** until the rubric's effect is measured on
   a fresh cassette sample (agreement rate/AUC rerun, no threshold
   change in this decision).
3. If the rubric still fails to produce a judge-correlated score, the
   follow-up is to gate on the judge itself (a cheap judge call per
   commit), which this decision deliberately does not yet authorize.

## Validation (2026-10-13, owner-approved rubric, measured)

The rubric was implemented and re-scored the 93 recorded critique calls
(no new candidates): **AUC 0.54** vs the target ≥ 0.75, **36% tied**
vs < 15%, **0% first-attempt ≥ 0.7** vs 80–90%. The anchors did not
break the critic's 0.15/0.35 quantization or raise its 0.62 ceiling —
the flash critic cannot be calibrated by prompting. Per this decision's
own escape hatch, the follow-up is to **gate on the judge itself**; see
`RUBRIC_VALIDATION.md`. Targets unmet — reported, not tuned.

## Decision needed

Acknowledge the measured failure of the rubric and approve the
follow-up direction (a per-commit judge call as the gate signal, a new
Proposed decision), or direct otherwise.
