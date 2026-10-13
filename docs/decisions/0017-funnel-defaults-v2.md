# 0017 — Funnel defaults v2 (W5 variants)

Status: **Proposed** (owner decision required; no defaults change until
accepted). Claimed by the OPT track; next free number at claim time.

## Context

The W5 funnel experiments (`FUNNEL_OPT.md`) ran the real funnel on the
seeded sample with one factor changed at a time (V1–V7, PR #33's
campaign-spec knobs), measured tokens/quality per variant and judged
quality blind-pairwise against the baseline children. The baseline's
measured behaviour (W5_BASELINE.md): kept {100,10,7,100} on the four
non-leaf paths, average critique score 0.12 (gate 0.7 unreachable), zero
cache hits, 33% review rate.

## Proposed defaults

| Parameter | Baseline | Proposed | Evidence (FUNNEL_OPT.md) |
|---|---|---|---|
| planner aspects | 4–8 | **3–5** | F2: −13/−26% tokens, 4/4 judge wins |
| trim rounds | 400 → 300 | **single 150** | F1: −44%/−44% tokens, score 0.12→0.29, 3/3 wins |
| shared prompt prefix (cache) | off | **on** | F3: nonzero cache hit measured, 3/3 wins |
| light funnel from | L8 | **L6** | F6: −49%/−48% tokens, 4.2× kept, score 0.27 vs 0.17 |
| worker candidates per aspect | 100 | 40 (accepted trade) | F5: −34%/−51% tokens, judge 1–2 |
| synthesizer range | 3..100 | 3..100 (unchanged) | F4: capping to 40 loses quality |
| output format | items objects | unchanged (compact optional) | F5 |

Everything ships as campaign-spec fields (PR #33); accepting this
decision means making these the defaults the engine uses when the
campaign file omits them. One-line config change; nothing else moves.

## Consequences

- Projected cost: the growth factor (children × non-atomic share) drops
  from the baseline's ~29–54 to roughly 10–25 at depth (F7 table), and
  per-node tokens fall ~35–50%; COST_MODEL.md's measured tables shrink
  correspondingly. Exact regeneration deferred until the owner's call.
- Risk: candidate/kept counts are lower, so mid-branch coverage leans on
  the critique's "add" list; the smaller samples (n=1–4 judged pairs)
  make these directions, not significances. The generalization run of
  the combined configuration (§W5.4) is deferred for budget.

## Decision needed

Accept (make proposed defaults effective), reject, or accept a subset —
each knobs independently behind the campaign spec.
