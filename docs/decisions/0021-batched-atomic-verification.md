# 0021 — Batched atomic verification (Proposed)

Status: **Accepted** (owner, 2026-10-13): implement behind
`verify_atomic`, default on once tested, measure the verified share.

## What the engine does today (verify_atomic off — the default)

`GATE_CALIBRATION.md` §2 plus this report:

- The synthesizer's `atomic` labels are recorded: `ATOMIC` log records
  on commit/needs-review, the settle's `atomic` column, and the
  review's `atomic` count (PR #36 fixed the plumbing so nothing is
  silently dropped).
- **They do not make leaves** per child: `ATOMIC` has no store effect
  in M3, so labelled children stay ordinary tickets and expand like
  any other node.
- **One mechanism did let them act as leaves:** the stop rule's
  atomic clause (last 50 settled ≥ 90% atomic and average score ≥ 0.8)
  counted the unverified labels — with the model labelling ~100% of
  kept children, a well-scored run could stop a campaign on raw label
  noise. Fixed: only *verified* labels count toward the stop rule
  (`verify_atomic` on; the sample is 0 otherwise), so unverified
  labels can no longer end expansion anywhere.
- The labels still feed reports and the projection's `atomic_share`
  (documented as raw-label values until verification is on).

## Proposal

Batched verification: **one call per committed parent** listing the
candidate children; the reply marks which children admit no further
decomposition. The funnel runs it instead of the per-child loop when
`verify_atomic` is on.

- Prompt: the parent title + the kept children (titles + the model's
  labels) → JSON `{"admits_none": [ ...titles... ]}`; children not in
  the list stay tasks. Fixed instruction text (cache-prefix friendly).
- Semantics unchanged from DESIGN › Quality gate: labelled atomic AND
  agreed by the check → leaf; otherwise a task.

## Cost estimate (W5.4 measurements)

Per-child today: 1 call (~150 tokens in, ~16 out) per labelled child —
at the measured ~70–100% label rate and 8–20 kept children that is
~6–20 calls per committed parent. Batched: **1 call** (~600 tokens in
with 20 titles, ~100 out) per parent — roughly a 6–20× reduction, or
one extra call per commit (~2% of the attempt's tokens at the V3
profile of 3,143 in / 5,043 out per attempt).

## Implementation and measurement (done)

The per-child loop is replaced by ONE call per attempt listing the
labelled children; the reply's `admits_none` names the verified leaves.
`verify_atomic` is ON by default; unit tests cover the named/unnamed/
failure/default paths and the strict parser. Engine-test fake updated.

**Measured on the W5 sample** (5 settled sets, 340 kept children, one
call each): **verified atomic share 0.49** (166/340) — per set
0.00→0.50/0.57/0.92/1.00 after fixing a truncation bug found by the
measurement itself (a 128-token reply cap cannot name 57 titles; the
call budget now scales with the list, `max(128, 10×labelled)`). The
growth factor halves post-verification: `children_kept × (1 − 0.49)`.
COST_MODEL.md was regenerated with the measured share.
