# 0020 — Branch shares: the pool ledger that makes a 50% share fold-deterministic

## Context

M8 · Depth and scale, item 1 (DESIGN › Work scheduling › Branch share). Each
scope root gets a pool of attempts; a child branch may spend at most 50% of
what its parent's pool has LEFT when the child starts; what leaves and short
branches do not use returns to the parent's pool for its siblings; a node
whose share cannot pay one attempt becomes a leaf (`SETTLE` with
`end = allowance spent`). The old L9 cap is removed and replaced by this
rule plus the optional depth checkpoint.

## Decision

1. **Starting pools.** When the `CAMPAIGN` record is written (and on every
   re-run of `campaign start` for a fresh id), the engine computes one pool
   per scope root, proportional to that root's OPEN ticket count (a root
   with no tickets gets one attempt); the pools are recorded IN the
   CAMPAIGN record's spec (`pools: {path: attempts}`) so every replica
   folds the same numbers regardless of when it learns the campaign. The
   pool is the OUTER cap per root: spending past it in that root's whole
   subtree never happens.

2. **The allowance fold.** A child's allowance is frozen when the child
   becomes a ticket. The fold processes ticket-creation events (the
   child's `ADD_NODE` record) in `(hlc, origin)` order — the same
   total-order rule as the RESERVE fold (0014) — and assigns
   `allowance(child) = floor(0.5 × remaining(parent))`, where
   `remaining(parent) = allowance(parent) − attempts_spent(subtree(parent))`
   at that point in the fold order. `attempts_spent` counts `ATTEMPT`
   records whose path is the parent or any descendant, in the same
   `(hlc, origin)` order. The ledger is a pure function of the log, so
   every replica computes identical allowances in every delivery order.
   A child with `allowance < 1` is a leaf immediately (no attempt spent).

3. **The engine check.** Before a ticket starts, the engine folds the
   ledger for its path (ancestor chain to the root). If the ticket's
   share has `attempts_spent ≥ allowance`, the engine writes
   `SETTLE` with `end = allowance spent` and no attempt — identical to
   how the cannot-split leaf works. The check runs per run-loop entry
   (cheap: the ledger is a small map; recompute on store changes, cached
   like the ticket index).

4. **Scope.** A ticket outside every scope root (stolen work at the
   campaign's frontier) uses its nearest scoped ancestor's pool; a scope
   root itself spends from its own pool with no allowance halving.

5. **Re-expansion.** A retried leaf (M8 leaves are reversible, DESIGN)
   takes the re-expansion lock and a FRESH allowance computed from the
   current remaining pool — the REEXPAND record carries the new
   allowance so the fold stays deterministic.

## Consequences

- `SETTLE.end` gains `allowance spent` (DESIGN already lists it).
- `CampaignSpec` gains `branch_share` (default 0.5, an exact decimal
  string per decision 0003) and `pools` (recorded in the CAMPAIGN spec).
- `sched` gains the pool ledger (`pool_ledger.cpp`-style pure functions +
  the fold in the state writer, replay-owned like the RESERVE entries).
- The old L9 guard is gone; depth checkpoints (M8 item 2) are the only
  depth coupling left, and they spend nothing.
- Tests: the agent-exit criterion "a single chain under a 50% share
  terminates" (a chain of one child per level ends within
  pool × Σ 0.5^k levels, no attempt wasted past the allowance); a broad
  tree shares the pool (a short sibling returns its allowance); a
  funnels-scenario campaign past L9 ends within its pools.

## Open (owner at M8 review)

- Parquet export (DuckDB) stays out — OA question only.
