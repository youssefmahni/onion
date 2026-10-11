# OPT track — PROGRESS

## Current state

| Field | Value |
|---|---|
| Workstream | W1 (setup and audit) |
| Branch | `opt/setup` → then `opt/w1-audit`, `opt/llm-probe` |
| Worktree | `~/Desktop/onion-opt` (never `~/Desktop/onion`) |
| Data copy | `~/onion-opt-data` (corpus copied and count-verified) |
| Last green CI | pending first push |
| Key source | environment (`DEEPSEEK_API_KEY` present, never printed) |

## Milestone state (read from `origin/main`, 2026-10-11)

M1–M5 merged and tagged `m1`…`m5-rc`; PR #12 `m6-sync` is open (Agent M).

## Real API usage (OPT track, caps: 1,500 calls / $5.00)

| Date | Purpose | Calls | Input (hit/miss) | Output | Cost USD | Running calls | Running USD |
|---|---|---|---|---|---|---|---|
| — | none yet | 0 | — | — | 0 | 0 / 1,500 | 0 / 5.00 |

Allocation (OPTIMIZATION_GOAL.md §9.3): W2 real check 300 · W3 smoke 10 · W5 1,000 · reserve 190.

## Known limitations (current)

- The corpus funneled-set claim is 12 nodes; the copy holds 11 `funnel_state.tsv` rows. Recorded in `AUDIT.md`; no action needed for W1.
- The corpus atomic-flag claim is 30; the copy holds 29 rows. Same note.
- Owner coordination note (§13 of the goal) must be relayed to the M6 agent by the owner (OA-1).
