# 0010 — Campaign liveness: exhausted vs finished

## Context

M6 runs one campaign across several devices. With the loop budget shared fleet-wide, a device can have no lane tickets while the budget is not yet exhausted, and a device can exhaust the budget while another still has an attempt in flight. The engine needs one precise definition of when a campaign stops making progress and when it is done.

## Decision (Accepted)

- **Exhausted.** A campaign is exhausted *on a device* when that device's fold of `RESERVE` records — counted in `(hlc, origin)` order, capped at the loop budget so overshoot is void — reaches the campaign's loop budget. An exhausted device starts no new attempts for that campaign.
- **Finished.** A campaign is *finished* when it is exhausted or stopped, and no device has an attempt in flight for it.
- **Exhaustion is not a stop.** Exhaustion does not close the campaign record (stop is final). Open tickets stay open, and if the loop budget is raised, the devices resume them.

## Consequences

- The engine returns `budget` without writing a terminal `CAMPAIGN` record; the `CAMPAIGN` record is closed only on the stop rule (or on a drain in single-device mode, where "nothing left to do" is a real finish).
- The multi-process convergence test asserts exhaustion plus quiescence (no new records for three sync rounds), then compares campaign state and Merkle roots — never raw per-run timing.
- `node run` sleeps between drained/exhausted re-entries instead of spinning.
