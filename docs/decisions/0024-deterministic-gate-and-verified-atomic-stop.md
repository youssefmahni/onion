# 0024 — Deterministic quality gate, optional critic, verified-atomic stop rule, audit sampling

Status: **Accepted** (owner: `GATE_CHANGE = approved`, 2026-10-14).
Supersedes: **0020** (critic rubric calibration, Proposed). Amends: **0008 §2**
(stop rule) and **0017** (gate defaults, which 0017 deferred to 0020).
Related: 0021 (batched atomic verification), 0003 (no floats in records).

## Context

Measured, not argued (REVIEW.md F-GATE-1; decision 0020's own validation):

- The deepseek-flash critic tops out at **0.62** (mean 0.31, quantized at
  0.15/0.35) against the 0.7 gate / 0.5 floor.
- Its score does not predict the blind judge: **AUC 0.54** (target ≥ 0.75),
  **36 % ties**, **0 %** of first attempts ≥ 0.7.
- With the shipped defaults every non-leaf node therefore burns three
  attempts and lands in review; the stop rule's score clause (mean ≥ 0.8
  over 50 nodes) is unreachable, so campaigns never stop on quality.
- The rubric fix proposed in 0020 was implemented and measured: it did not
  help ("the flash critic cannot be calibrated by prompting").

A score that carries no signal must not decide anything. The deterministic
checks the engine already runs do carry signal (repeats, style, size,
completeness), and they are free.

## Decision

1. **The gate is the deterministic checks.** An attempt is **committed**
   when all of these pass:
   - the reply is complete (`Kind::Ok`, not truncated) and valid JSON;
   - the child count is within the campaign target: at least
     `children_min` (default **8**) and at most `children_max` (default
     **30**; the prompt asks for 8–20, 30 is the hard maximum, no padding);
   - no child repeats the node, an existing child, a sibling, an ancestor
     or another child of the same reply, and none is too similar to the
     parent (similarity ≥ 0.85) or to a higher ancestor (≥ 0.95);
   - every title passes the style gates (`style_flags`).
2. **Retry only when a check fails**: up to `max_attempts` (default 3) at
   temperatures 0 / 0.3 / 0.5. The failure reasons of the previous attempt
   (every failed check, by name and offending title) are injected into the
   next attempt's planner and synthesizer prompts — the synthesizer is where
   the titles are written, so that is where the reasons matter.
3. **Fewer than three valid children is a leaf.** A *valid* child is one
   that passes the per-title checks (repeat, similarity, style). When the
   attempts are exhausted without a passing attempt, the attempt with the
   most valid children (ties: the later one) commits **its valid children
   only**, recorded as "below target" in the SETTLE when fewer than
   `children_min`; if no attempt has at least three valid children the node
   is a leaf (`cannot split`). Provider failures are **not** leaves
   (F-GATE-2): they defer the node (decision outcome `Defer`, fixed in B1).
4. **The critic is optional**: campaign field `critic: "on" | "off"`,
   default **off**. Off means no critique call and no CRITIQUE record. On
   means the critique runs before synthesis as today, its score is
   recorded and its remove/add/issues text feeds the synthesizer, but it
   **never gates** a commit and never drives a retry. `critic_model` keeps
   its meaning for the critique call only. The `gate` and `floor` fields
   remain parseable for old campaign files and are ignored (they round-trip
   unchanged in the CAMPAIGN record).
5. **Step order**: pre-check → planner → workers → local near-duplicate
   sweep → **one** trim to the trim target (skipped when the deduplicated
   pool is already at or below it) → [critique, only when `critic: on`] →
   synthesizer → deterministic checks on each attempt → choose the attempt
   to commit → **one batched atomic verification on the chosen attempt
   only** (decision 0021 batching kept; rejected attempts no longer pay for
   a verification call).
6. **Stop rule — verified atomic share only.** Over the last **50**
   settled expansion nodes of the campaign (settles with `end = children`;
   leaves and reviews do not count), the pooled verified-atomic share
   `Σ atomic children / Σ children` is computed; the campaign stops when
   at least 50 such nodes exist and the share is ≥ **0.90** (campaign
   field `stop_atomic_share`, decimal string, default `"0.90"`). The score
   clause is removed. The window is derived from the store (SETTLE rows in
   hlc order), so it survives restarts and is identical on every replica
   (0008's in-memory window did not survive re-entry).
7. **Audit sampling**: campaign field `audit_rate` (decimal string, default
   `"0.02"`). Each committed node is sampled deterministically — the
   BLAKE2b hash of `campaign ‖ path` taken modulo 10⁶ is below
   `audit_rate × 10⁶` — so every replica samples the same nodes. A sampled
   node produces an `AUDIT` record: a **non-blocking** review item (the
   subtree keeps expanding) shown in the review queue with reason `audit`.
   `/api/state` reports `audits: {open, sampled}` per campaign. Resolving
   an audit records the verdict only (`accept` = looks fine, `reject` =
   looks wrong; a follow-up is the operator's `review retry`).
8. **Defaults summary** (campaign file, all optional): `critic: "off"`,
   `children_min: 8`, `children_max: 30`, `audit_rate: "0.02"`,
   `stop_atomic_share: "0.90"`, `max_attempts: 3`.

## Consequences

- Campaigns commit autonomously, and stop when the frontier is atomic.
- Each node saves the critique call (one of ~8) and every rejected
  attempt's verification call; the cost model (decision 0016, COST_MODEL.md)
  is regenerated from the B4 measurements, not from this reasoning.
- Quality control moves from a score nobody can trust to (a) deterministic
  checks and (b) a **2 % human audit sample** with a visible queue. If the
  audit rejects more than a few percent, the owner tightens the checks or
  turns the critic on to *inform* the audit — not to gate.
- `NEEDS_REVIEW "below floor"` is no longer produced by the funnel; the
  remaining blocking reviews are "structure disallowed" and "depth
  checkpoint" (decision 0022).
- A judge call per commit (0020's escape hatch) is **not** adopted here.
  If the audit shows the checks are too lax, that is the next decision.

## Tests (each rule)

Deterministic commit; retry only on check failure with reasons injected;
below-target fallback and the three-valid-children leaf rule; no critique
call when `critic` is off, critique recorded but non-gating when on; trim
skipped at or below the target; verification only on the chosen attempt;
stop rule on the verified atomic share only and surviving a restart; audit
sampling determinism, rate and non-blocking behaviour; `/api/state` audit
counts; campaign file round-trip of the new fields.
