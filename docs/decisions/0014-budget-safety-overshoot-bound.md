# 0014 — Budget safety: the overshoot bound that actually holds

## Context

Decision 0010 states that a campaign's loop-budget overshoot is "at most one chunk per device". M6's implementation did not guarantee it: a device could emit a `RESERVE` for every chunk it consumed between sync rounds (its own fold never saw the peers' reservations until the next round), so with a 15 s sync lag a fleet could overshoot by many chunks, not one. The OPT review of the M6 convergence work (docs/agent/opt/reviews/m6-convergence.md) flags this, and the owner approved this amendment.

## Decision (Proposed → owner approved 2026-10-11)

1. **Capped fold.** `RESERVE` records fold in `(hlc, origin)` order, capped at the campaign's loop budget. Records past the budget are **void**: they count for nothing, and a device that learns its own reservation is void starts no new attempts for that campaign (attempts already in flight finish).
2. **One reservation per device per sync round.** A device may reserve its next chunk only after a completed sync round since its previous `RESERVE` (its first reservation is free). In single-device mode there are no sync rounds; the device keeps its current chunk discipline.
3. **Measured chunk size.** In fleet mode the chunk is `max(50, measured_attempts_per_sync_round × 1.2)`, measured as the attempts the device finished in its previous sync-round window, so fast devices are not throttled by a fixed 50. The campaign's `chunk` field remains the floor for single-device runs.
4. **The real bound.** The fleet-wide overshoot is therefore at most one chunk per device — about one sync round of spending per device, not per attempt.

## Consequences

- The state writer keeps each campaign's `RESERVE` entries as `(hlc, origin, attempts)` and derives the capped total and the void set from the sorted order, so every replica folds the same total and voids the same records regardless of delivery order.
- The engine exposes `note_sync_round()`: the node loop calls it after each completed sync round, the engine measures the window, and the next chunk uses the measured size.
- DESIGN.md's budget text is updated from "overshoot is at most one chunk per device" (aspirational) to the bound this rule makes true: at most one sync round of spending per device.
- Tests: a deterministic simulation of 3 devices at ~15 attempts/s with a 15 s sync lag and a budget of 1,000 asserts the total spent never exceeds budget + one chunk per device in every delivery order, that a device waits for a reservation less than 10% of the time while budget remains, and that void detection stops new attempts within one round.

## Amendment (owner-asked clarification, 2026-10-13)

**"Completed sync round" is any-answered, not all-answered.** The node
loop marks a round complete when at least one peer answered within its
timeouts (3 s connect, 5 s read/write per peer). A slow or unreachable
peer can never stall reservations: its request times out in bounded time
and the round still counts. A fully partitioned round (no peer answered)
does NOT count, so a device cut off from the fleet cannot unlock more
spending. Implemented in `src/cli/commands.cpp` (the sync loop sets
`any_peer_answered` and calls `engine.note_sync_round()` only when true).
