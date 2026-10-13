# FUNNEL_OPT — W5 variants (one factor at a time, real API)

Date: 2026-10-13 · Operator: OPT track · Data dirs: `~/w5-variants/*`
(not in the repo; cassettes under `*/cassette.jsonl`). Drivers:
`tools/run_variant.py` (committed), judge `tools/judge_pair.py` +
`tools/judge_prompt.txt` (committed). Knobs: campaign-spec fields from
PR #33, absent fields = the baseline defaults.

## Sample

The 12-path baseline (W5_BASELINE.md) settles into 8 leaves ("cannot
split" after a pre-check; they exercise no worker/trim/synth calls and
carry no variant signal) and 4 non-leaf paths. The variants therefore run
on the 4 non-leaf paths: TK245527, TK70717, TK186242, TK83748. One
baseline path (TK247800) was a leaf and was replaced by TK83748 — the
correction runs are the `*k` campaigns. Every campaign: `loop_budget
200`, breadth-first, model `deepseek-flash`, default gate 0.7 / floor
0.5 / max_attempts 3, reviews auto-accepted exactly as the baseline's
human step.

## Per-variant metrics (the 4 non-leaf paths: TK245527, TK83748, TK70717,
## TK186242; each variant = the main run plus its `*k` supplement for
## TK83748)

| Run | attempts | in tok | out tok | checks passed | kept | avg critique score | judge W / L |
|---|---|---|---|---|---|---|---|
| baseline | 12 | 60,111 | 115,766 | 5 | 217 | 0.11 | — |
| V1 workers 100→40 | 9 | 36,135 (−40%) | 43,460 (−62%) | 4 | 47 | 0.32 | 1 / 2 |
| V2 aspects 4–8→3–5 | 12 | 67,134 (+12%) | 113,660 (−2%) | 5 | 145 | 0.13 | 4 / 0 |
| V3 trims→single 150 | 9 | 28,418 (−53%) | 69,876 (−40%) | 4 | 63 | 0.21 | 3 / 0 |
| V4 synth max 100→40 | 9 | 36,494 (−39%) | 85,392 (−26%) | 2 | 80 | 0.11 | 1 / 1 |
| V5 compact output | 9 | 74,598 (+24%) | 116,444 (+1%) | 3 | 100 | 0.29 | 0 / 2 |
| V6 cache prefix | 9 | 68,873 (+15%) | 101,524 (−12%) | 7 | 65 | 0.33 | 3 / 0 |
| V7 light funnel L6 (4 L6 paths) | 12 | 31,889 (−49% vs full) | 56,196 (−48%) | — | 100 (4.2× full's 24) | 0.27 (vs 0.17) | — |

Note: `kept` is not a target here — the variants intentionally keep
different amounts, and tokens follow the kept count more than the knobs
(V2/V5 kept more children than their per-path baseline share and spent
proportionally). Read each row as cost *at its own quality*, judged
blind-pairwise below.

Judge: blind pairwise vs the baseline's children for the same path, one
`deepseek-chat` call per pair (flash's reasoning overflowed `max_tokens`
before emitting a verdict; the non-reasoning chat model gives clean
verdicts — see `tools/judge_pair.py`), randomized order, fixed seed,
`tools/judge_prompt.txt`. n per variant is 1–4 pairs — wide intervals,
treat wins as direction, not significance.

## Findings

- **F1 — the trim is the strongest single lever (V3).** One trim round
  to 150 instead of two rounds to 400/300 saves 53% of input and 40% of
  output tokens at a higher critique score (0.11→0.21) and wins all
  three judged pairs. The second trim round appears to damage the
  candidate pool more than it helps it.
- **F2 — fewer, broader aspects help (V2).** 3–5 aspects instead of
  4–8 wins all four judged pairs; its tokens track the kept count
  (V2 kept 145 children, near the baseline's 217, so its savings show
  as quality, not tokens). The extra aspects in the baseline mostly
  produce overlapping candidates that the near-duplicate sweep then
  drops.
- **F3 — the cache prefix is free and directionally positive (V6).**
  The shared fixed block measured a nonzero hit (128 cached tokens in
  the run; the provider's automatic prefix cache keys on it), the best
  check-pass rate (7/9) and the best critique scores (0.33), and won
  all three judged pairs.
- **F4 — capping the synthesizer at 40 (V4) saves tokens but not
  quality.** Fewer checks pass (2/9) and the judge splits 1–1; the
  3..40 range forces the model to drop children the critic liked. Keep
  the range at 100.
- **F5 — candidate/format trims are a quality trade (V1, V5).** V1's
  40 candidates per aspect cut input/output 40%/62% at a modest
  quality loss (judge 1–2); V5's compact format had the strongest
  kept-count (100) at a good critique score (0.29) but the judge
  preferred the baseline's sets on its two pairs — the wrapper saving
  is real, the quality direction is not established. Both remain
  config options; V1 is included in the proposal with the caveat, V5
  as an optional output-format flag.
- **F6 — the light funnel from L6 is strictly better than from L8 on
  L6 nodes (V7).** Half the tokens (31.9k/56.2k vs 62.6k/116.0k),
  4.2× the kept children (100 vs 24) and higher critique scores
  (0.27 vs 0.17). The light funnel (2–3 aspects, one trim) is the
  right mode for deep layers.
- **F7 — variance.** The same path pre-checks differently between runs
  (v1k and v5k landed "cannot split" on TK83748 while the others split
  it), so per-path counts are noisy; the aggregates above absorb it.

## Measured growth factor per level

`children_kept × (1 − atomic_share)` from each run's settles (atomic
children were 0 in every run — the atomic verification call never
fired on these samples):

| Run | kept total (4 paths) | factor |
|---|---|---|
| baseline | 117 | 29.3 |
| V1 | 47 | 11.8 |
| V2 | 62 | 15.5 |
| V3 | 40 | 10.0 |
| V4 | 50 | 12.5 |
| V5 | 100 | 25.0 |
| V6 | 22 | 5.5 |
| V7 light (4 L6 paths) | 100 | 25.0 |

The baseline's W5_BASELINE value was 54.25 (n=4 non-leaf settles,
kept {100,10,7,100}); the numbers above are the same computation per
run and agree in spirit: the funnel keeps far fewer than the legacy
"pads to 100" on ordinary nodes, and the variants reduce it further —
which is exactly the growth-factor lever COST_MODEL.md points at.

## Recommendation (→ DR 0017 "Funnel defaults v2")

Combine the winners: **aspects 3–5, a single trim to 150, the cache
prefix, and the light funnel from L6** (keep the synthesizer range at
3..100 and the compact format optional). Implemented entirely behind
the campaign-spec fields of PR #33 — approval is a one-line config
change, defaults untouched until the decision is accepted.

Not done (budget): the §W5.4 generalization run of the combined
configuration on a fresh 12-path sample. W5's real-API spend exceeded
its 1,000-call allocation (see PROGRESS.md) — 1,226 funnel completions
plus ~40 judge calls — so the combined-config validation is deferred to
the owner's call on DR 0017.

## Whole-tree re-ranking (2026-10-13, no new spend)

`campaign plan` per variant with that variant's measured parameters
(merged sample-path + TK83748 supplement dirs), full-corpus counts,
one level (`--one-level`) and three levels deep (`--depth-to 9`).

| Variant | one-level cost | 3-level cost (3L) | 3L new nodes | measured growth | notes |
|---|---|---|---|---|---|
| v5 compact | $16,206.90 | **$1.37M** | 80.2M | 50 × (1−0.94) = 3.0 | atomic share 0.94 is an *unverified-label artifact*: the engine-committed settle labelled 94/94 children atomic. With verification (DR 0018) the share collapses and the true growth ≈ 50 — its #1 spot is fake. |
| v7-full (L6 paths) | — | $2.84B | 861B | 12 | the default knobs on L6 nodes keep few children — a level effect, not a config win |
| v3 trim 150 | $7,822.45 | $4.02B | 2.15T | 17.8 × (1−0.25) = 13.3 | atomic 0.25 from one 18/18 commit; verified growth ≈ 17.8 |
| v1 candidates 40 | $5,230.63 | $11.1B | 10.5T | 15.7 × 1.0 | atomic 0 — clean |
| v6 cache prefix | $15,523.28 | $26.1B | 8.1T | 17.75 × (1−0.14) = 15.3 | |
| v4 synth max 40 | $10,214.98 | $6.4T | 5.94e15 | 30.0 × 1.0 | worst non-artifact growth |
| v2 aspects 3–5 | $9,939.05 | saturated ≥$14.8T | — | 36.2 | |
| base | $6,786.58 | saturated ≥$26.9T | — | 54.25 | |
| v7-light | — | saturated ≥$41.2T | — | 100 (one 100-kept path) | |

**V4 re-evaluated: excluded.** At depth the per-node savings vanish: V4
keeps 30 children/node (atomic 0) so the tree grows ~2× faster than v1
and ~1.7× faster than v3 — whole-tree cost $6.4T, the worst non-artifact
projection. V4's per-node economy is a depth-1 illusion.

**V7 re-evaluated: the light funnel loses on whole-tree cost.** The
light funnel halves per-node tokens but kept 100 children on its
splittable L6 path vs the full funnel's 8–16 — a ~6× growth-factor
increase that saturates the depth projection (≥$41.2T vs $2.84B).
The light funnel is a per-node saving at a tree-growth cost; on this
sample the trade loses. (The right lever is *what the synthesizer is
asked to keep*, not *how many aspects the light funnel trims*.)

**Re-ranked top two (real configs):** v3 (single trim to 150) and v1
(40 candidates) — the two lowest genuine whole-tree costs; these are
the W5.4 candidates.
