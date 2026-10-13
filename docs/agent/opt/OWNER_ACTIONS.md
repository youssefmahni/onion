# OPT track — OWNER_ACTIONS (append-only; newest first)

Format from OPTIMIZATION_GOAL.md §10: each entry is executable without asking the agent anything.

---

### OA-1 · Relay the M6 coordination note · W1 · status: open

Why: OPTIMIZATION_GOAL.md §13 contains a short coordination note for the agent
building M6 (PR #12, branch `m6-sync`). It tells that agent that a second agent
works in `~/Desktop/onion-opt` on `opt/*` branches, which files never to touch,
and to read `docs/agent/opt/COORDINATION.md` at the start of each session.

Prerequisites: the session in which the M6 agent runs.

Steps:
1. Paste the note from §13 of `docs/agent/opt/opt/OPTIMIZATION_GOAL.md`
   (in the repo: `docs/agent/opt/OPTIMIZATION_GOAL.md`, section "13. Coordination
   note for Agent M") into the M6 agent's session, unchanged.
2. Confirm to the OPT agent that it was delivered (or paste the note into
   `docs/agent/opt/COORDINATION.md` yourself as an entry).

Pass criteria: the M6 agent acknowledges the four numbered points.

If it fails: record the M6 agent's response in `docs/agent/opt/COORDINATION.md`.

Cost / duration: no API calls; under 5 minutes.

### OA-2 · Fix GitHub Actions billing (CI blocked) · all workstreams · status: done (2026-10-11: the repo was made public, which restored free Actions minutes; CI green since)

Why: Every CI job now fails before starting: "The job was not started because
recent account payments have failed or your spending limit needs to be
increased." (first seen 2026-10-11 ~19:45 UTC, run 38169437593). No PR can
merge (merge rules require CI green), and both agents' verification depends
on CI.

Prerequisites: access to the repo owner's GitHub Billing & plans settings.

Steps:
1. Open https://github.com/settings/billing (organization or personal, as
   applicable for youssefmahni/onion).
2. Fix the failed payment or raise the spending limit.
3. Re-run one failed job (`gh run rerun 38169437593 --failed`).

Pass criteria: the re-run jobs start and complete (pass or fail on their own
merits, not on billing).

If it fails: capture the billing page state and send it to the agents.

Cost / duration: none; minutes of clicking.

### OA-3 · Two-network concurrency test · W6 (F6) · status: open

Why: finding F6 from OPTIMIZATION_GOAL.md §7 — does throughput double when
two machines on DIFFERENT networks run the probe at the same time (limits
per machine), or does it stay at one machine's level (limits per account)?
Decides whether a fleet of N machines needs N keys.

Prerequisites: two machines on different networks (home Wi-Fi + phone
hotspot, or a cloud VM), each with a built `deepseek_probe` binary and a
key, and a way to start them within ±2 s (shared clock/alarm).

Steps:
1. On each machine, at the agreed time:
   `./build/linux-debug/deepseek_probe --base-url https://api.deepseek.com/v1
    --model deepseek-flash --duration 600 --in-flight 600 --jsonl probeX.jsonl`
   (X = A on one machine, B on the other).
2. Take each probe's plateau summary (steady-state completed output
   tokens/s over the last 300 s).
3. Compute (rateA + rateB) / rateA_alone. ≥ 1.8 → per-machine limits (the
   fleet scales with machines); ≤ 1.2 → per-account (each machine needs its
   own key). Report the ratio to the OPT agent (e.g. one line in
   COORDINATION.md), plus any 429 ceilings the two machines hit.

Pass criteria: a reported ratio with the two per-machine rates and the
time-sync method used.

If it fails: report the error/limit behaviour; the hard-backoff evidence
is itself informative.

Cost / duration: 10 minutes wall clock; ~300–500 tiny probe calls per
machine, well under $0.05 total. Excluded from OPT's workstream
allocations (owner-run).
