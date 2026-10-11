# Read-only second opinion: M6 multi-process convergence (W7)

Scope: OPTIMIZATION_GOAL.md §6 W7. Nothing in Agent M's area was changed;
every claim is from reading the code on `origin/main` (post-M6 merge,
`1f5176a`, and `68ad2cb`) or from running the existing test read-only.

## 1. The test's "finished" versus DESIGN.md

The test (`tests/unit/multi_process_test.py`, `finished_everywhere`,
lines 153-167) considers a device finished when its `/api/state` campaign
says `state == "stopped"` **or** `reserved >= loop_budget`, where
`entry["reserved"] = writer.reserved_total(campaign.id)`
(`src/cli/commands.cpp:146`).

- The "stopped" arm matches decision 0010.
- The exhaustion arm deviates in one respect: `reserved_total` is the
  **sum of every synced RESERVE record**, deduplicated by `(origin, hlc)`
  (`src/state/state.cpp:532-541`). Decision 0010 defines exhaustion as the
  RESERVE fold *counted in (hlc, origin) order, capped at the loop budget so
  overshoot is void*. The implementation has no ordering, no cap and no
  void: two devices reserving the last chunk concurrently both count, and
  `reserved_total` can report more than the budget (up to one extra chunk
  per device, which 0010 allows as overshoot but defines as *void*, not as
  part of the fold). Practical effect on the test: the `>= budget` check
  still passes; the deviation matters once a device must decide whether *its
  own* reservation is void (it never does today — every device spends its
  own chunk in full).
- Also note the test's `finished_everywhere` does **not** check the second
  half of 0010's "finished" (no device has an attempt in flight). The
  subsequent quiescence phase (3 sync rounds with identical vectors) covers
  it indirectly: an in-flight attempt writes its ATTEMPT/terminal batch and
  resets the quiescence counter. That is correct but slow (one extra 15 s
  round per straggler).

## 2. Candidate liveness bugs, each with confirming/refuting evidence

### L1. RESERVE fold lacks 0010's order/cap/void — CONFIRMED (deviation)
Evidence: `src/state/state.cpp:532-541` sums with a seen-key set; no sort by
`(hlc, origin)`, no min(budget) cap, no voiding. `reserved_total`
(`state.cpp:890`) returns the raw sum. This cannot make the test fail (the
check is `>= budget`), but M7/M8 work on budget chips, spillover and
exhaustion detection should not trust the fold's value to be exactly the
budget.

### L2. Budget-exhaustion detection across nodes — SOUND, but sync-latency sensitive
Evidence: a device with empty local tickets and `reserved_total < budget`
ends its campaign pass with `end_reason = "drained"` and sleeps 250 ms
(`src/sched/sched.cpp:346-356`, `src/cli/commands.cpp:654,692`); the RESERVE
records arrive through one random peer per 15 s sync round
(`src/sync/sync.cpp:324+`). With 3 devices, the campaign record and the
RESERVEs from device 1 reach device 3 in up to 2 rounds (~30 s). The test's
work window is `max(60, deadline − 90)` s (line 171). The mechanism is
liveness-correct; the margin is timing, not logic.

### L3. Lane ownership of tickets that remain open — SOUND for this test
Evidence: each device erases every ticket whose `lane_owner != self`
(`src/sched/sched.cpp:330-344`); `lane_owner` is deterministic HRW with a
device-id tie-break (`sched.cpp:626-641`). All 3 devices are live with equal
weights, so every ticket has an owner that eventually works it. A ticket
whose owner is stalled (slow sync) simply waits; nothing deadlocks. The
single risk: if a device's own lane work depends on genesis records it has
not synced yet, it drains and retries — bounded by the sync cadence.

### L4. Quiescence detection — SOUND, one subtlety
Evidence: the test compares the full `vector` map on all 3 devices every
15 s and needs 3 identical consecutive snapshots (lines 178-196). Any record
anywhere resets the counter. The subtlety: `finished_everywhere` can be true
while a device still has an in-flight attempt (see §1), whose terminal batch
then lands inside the quiescence window — correct, just slower. No bug.

### L5. Structure flag parsed but never enforced — CONFIRMED (deviation,
### load-bearing for this test)
Evidence: `spec.structure` is parsed (`src/sched/sched.cpp:57`) and never
read again (the only occurrence is line 57). The ticket order ignores it, so
L1-L3 nodes are expanded regardless. The multi-process test depends on this:
its corpus is one L0 per device, and with structure enforcement (DESIGN:
L1-L3 expandable only when the campaign allows it, results always reviewed)
the campaign would settle only the 3 L0 roots, reserve one chunk of 50 and
never reach the 100-attempt budget — "campaign never finished everywhere"
would become the *expected* outcome. M7/M8 must either enforce the flag and
redesign this test's corpus, or keep the current behavior deliberately.

### L6. The flake itself — TIMING, not liveness (best available evidence)
Observed locally: the test passed 6/6 isolated runs and 3/3 after the stale
servers were removed; it failed 2 of 6 full-suite rounds *while other
tests ran in parallel*, and a stale fake server from an aborted earlier run
was found holding port 18101 (which the test's fake server also binds —
`multi_process_test.py:91`, fixed ports 18101/18110-3/18201-3). When that
port is taken, the new fake server fails to bind silently and the campaign
runs against a foreign server — a deterministic-looking failure that is
actually environmental. On CI the runners are fresh (no stale processes),
so the CI flake is the load-margin variant: the 60 s work window vs the 15 s
sync cadence and 3 shared processes under a loaded runner. The failure dump
("campaign never finished everywhere" plus per-device `debug-state`) would
confirm: expect `reserved < 100` on at least one device at the deadline, or
one device's sync lines lagging.

## 3. Deterministic reproduction recipe (in-process fleet harness)

The in-process fleet harness (`tests/fleet/`) does not exist yet on main;
the multi-process test is the fleet harness today. Recipe to drive the
timing flake:

1. Free ports first (the test binds fixed ports):
   ```bash
   ss -ltnp | grep -E '18101|1811[0-3]|1820[1-3]'   # must be empty
   ```
2. Run under load, matching the CI shape:
   ```bash
   ctest --test-dir build -j8            # or: parallel ctest with the
                                         # llm.* tests as co-tenant
   ONION_MP_DEADLINE_S=60 python3 tests/unit/multi_process_test.py build/data_depot
   ```
   The tightened `ONION_MP_DEADLINE_S` compresses the work window and makes
   the failure reproducible in ~1 minute instead of ~2.
3. On failure the test prints per-device `node debug-state` and log tails;
   the diagnosis is: which device has `reserved < loop_budget`, and whether
   its `sync:` lines show rounds to its peers succeeding (liveness) or not
   (environment/auth).
4. To rule out the stale-port failure mode, rerun step 1 after any kill.

Suggested M7 hardening (for Agent M, not done here): pick ports with port 0
and thread them through, and raise the work-window margin on CI (the current
`max(60, deadline−90)` leaves 45 s for 3 quiescence rounds only if work
finishes in the first 60 s).

## 4. Is OA-13 (macOS sync 401) related?

Not directly. OA-13 is a *wholesale* authentication failure on the hosted
macOS arm64 runners: every round is rejected before any sync happens, so the
test skips there (exit 77). The Linux flake fails *after* successful sync,
in the work phase, with the "campaign never finished" mode; a 401 on Linux
would show `vector request failed (status 401)` in the failure dump's sync
lines instead. Two shared threads make it worth one joint look in M7:
(a) the request-signature path (`src/sync/sync.cpp` authenticate) is the
same code both symptoms pass through, and (b) OA-13's own steps ask for the
rejection reason to be reported in the 401 body — adding that reason helps
the Linux diagnosis too. If the macOS fix lands and the skip is removed, the
macOS CI run will additionally face the same 60 s work-window margin as
Linux; the ONION_MP_DEADLINE_S tightening above is the fastest way to
pre-check that.

## Summary for the owner

The convergence test's logic is sound; the flake is the work-window margin
under load, plus a stale-port footgun locally. Three real deviations from
decision 0010/DESIGN are documented above for M7/M8 (RESERVE fold order/cap,
the structure flag, and the missing in-flight clause of "finished" in the
test). OA-13 is a separate, macOS-only authentication failure.
