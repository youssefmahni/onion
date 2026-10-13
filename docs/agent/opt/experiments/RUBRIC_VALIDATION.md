# DR 0020 validation — the anchored rubric did not calibrate the critic

Date: 2026-10-13 · Critique-only re-scoring (no new candidate
generation), within the 4,000-call cap.

## What ran

The rubric critique prompt (fixed 0.8/0.5/0.2 anchor examples) re-scored
all 93 unique recorded critique calls (W5 + W5.4 cassettes,
`tools/rescore_critiques.py`, temperature 0, `reasoning_effort: none`
as the funnel sends; the recording proxy enforced the caps).

## Results vs the targets

| Target | Measured | Verdict |
|---|---|---|
| AUC ≥ 0.75 vs the blind judge | **0.54** (5 agree / 4 disagree / 5 tied over the judged pairs with both sides re-scored) | ✗ |
| tied scores < 15% | **36%** | ✗ |
| 80–90% of non-leaf nodes pass at 0.7 on the first attempt | **0%** (0/42 first-attempt critiques ≥ 0.7) | ✗ |

The re-scored distribution is essentially the pre-rubric one: modes
0.15/0.35 (60 of 93 at 0.35), max 0.62 — the anchors did not break the
model's quantization and did not move the ceiling. The deepseek-flash
critic cannot be calibrated to the 0.7/0.5 gate by prompting alone.

## Conclusion (per DR 0020's own escape hatch)

The rubric direction is measured and insufficient. The next step is the
one DR 0020 reserved: **gate on the judge itself** — a cheap judge call
(deepseek-chat, which produced clean, discriminating verdicts for W5.4
at 12/0 and 8/4) per committed attempt, replacing the critic score as
the gate signal. That requires a new decision (DR 0022, Proposed below
is a stub — file it after the owner's call). No thresholds are changed;
the gate remains 0.7/0.5 and unreachable by the critic until then,
which means every non-leaf node continues to flow to review.
