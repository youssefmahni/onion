# 0018 — Gate recalibration for the deepseek-flash critic (Proposed)

Status: **Proposed** (owner decision required). Claimed by the OPT track;
next free number at claim time.

## Context

`GATE_CALIBRATION.md` (zero-spend cassette analysis): the critic prompt
and parser are faithful (88/88 replies clean 0–1 JSON, same raw-pool
semantics as the legacy critic), but the deepseek-flash critic tops out
at **0.62** while the gate defaults (`pass 0.7 / floor 0.5`, DESIGN) were
calibrated for the legacy critic (p75 0.72, 29.7% ≥ 0.7). Result: zero
gate passes on every W5 run; 33% review rate; every non-leaf node waits
for a human.

The blind-judge winners' critique scores (n=17): min 0.15, median 0.35,
max 0.62. The recalibration below keeps the winners committable and the
sub-worst-winner attempts in review.

## Proposed defaults

| Parameter | Current | Proposed | Rationale |
|---|---|---|---|
| `gate` (pass score) | 0.7 | **0.35** | median judge-winner score; current critic mode |
| `floor` | 0.5 | **0.15** | min judge-winner score (inclusive) |

Both are campaign-spec fields already; accepting this decision makes the
proposed values the defaults `parse_campaign` uses when a campaign omits
them. The stop rule's own thresholds (avg score ≥ 0.8 over a window —
DESIGN › Stop rule) are not changed here.

## Companion findings (same evidence, decided separately if desired)

1. **Atomic verification is dead code** in M3 and is now wired behind
   `verify_atomic` (off by default; the per-child sweep costs ~0.7×kept
   calls at the model's ~69% label rate — DR 0017's economics note).
2. **The model over-labels atomic**: 100% of kept children on the
   engine-committed settles were labelled atomic (18/18, 94/94, 10/10);
   the legacy funnel flagged 30 total. Post-verification shares are
   expected to collapse; until then `atomic_share` projections using raw
   labels understate tree growth.
3. **`parse_critique` hardened**: scores outside [0, 1] now fail the
   attempt instead of silently mis-scoring (test from recorded replies).

## Decision needed

Accept the recalibrated defaults, adjust them, or reject. W5.4 runs with
the proposed values regardless (campaign-file override) so the owner's
accept/reject can compare the measured gate pass rate against the
recorded 0%.
