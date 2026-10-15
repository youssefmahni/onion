# Decision 0028 — Remove audit sampling from the engine

Status: Accepted (owner directive 2026-10-15)

Renumbered from 0026: the dashboard record (0026-dashboard-frontend-
remote-bind) claimed that number in main first, and a second record
with the same number is a defect (decision INDEX rule 1).

## Context

Decision 0024 (B3) added deterministic audit sampling: the engine
samples `audit_rate` of committed nodes (BLAKE2b(campaign ‖ path) mod
10^6) and emits non-blocking `AUDIT` review items for the operator to
spot-check. On the live two-Mac-Pro fleet the owner reviewed the queue
and directed its removal from the engine.

## Decision

The engine no longer emits audit samples:

- `audit_rate` is removed from `CampaignSpec`, from `parse_campaign`
  (old files that carry the field parse fine; the field is ignored) and
  from `campaign_spec_json`.
- The sampling emission and `audit_sampled()` are deleted.
- The `review_accept`/`review_reject` audit shortcuts are deleted (they
  handled audit items' verdict-only semantics).
- The `/api/state` per-campaign `audits` object is removed.

The StateWriter still folds legacy `AUDIT` records (replay of existing
logs stays clean and existing open audit items stay closable through the
review flow), but no new ones are ever written.

Supersedes: 0024's audit-sampling clauses (the rest of 0024 — the
quality gate and funnel fixes — stands).

## Consequences

- The review queue only ever carries blocking items (depth checkpoints).
- The operator loses the built-in spot-check queue; quality oversight
  relies on the dashboard quality-guard metrics instead.
