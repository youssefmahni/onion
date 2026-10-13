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
| 2026-10-11 | W3 C5 real smoke: GET /models (free) + minimal completion + truncation (max_tokens=1) + invalid-key 401 (unbilled) | 2 billed | 0 hit / 32 miss | 2 | ~$0.0000066 | 2 / 1,500 | ~0.000007 / 5.00 |
| 2026-10-13 | W5 baseline (w5b3–w5b8, 12 seeded paths, real funnel; usage from the proxy cassette, 135 completions) | 135 billed | 0 hit / 86,904 miss | 127,532 | ~$0.14 | 137 / 1,500 | ~$0.14 / 5.00 |
| 2026-10-13 | W5 variants V1–V7 + k-supplement + blind judges (FUNNEL_OPT.md; ~1,226 funnel completions + ~40 judge calls; several wasted flash-judge calls not counted) | ~1,266 billed | ~0.6M | ~0.8M | ~$1.05 | ~1,403 / 1,500 | ~$1.19 / 5.00 |

Note: W5 exceeded its own 1,000-call allocation (§9.3) by ~266 calls —
the TK247800→TK83748 scope correction (k-supplements) and the V7 pair.
The hard caps (1,500 calls / $5.00) still hold. No further real-API
spend without the owner's call; W5.4 generalization deferred (DR 0017).

Allocation (OPTIMIZATION_GOAL.md §9.3): W2 real check 300 · W3 smoke 10 · W5 1,000 · reserve 190.

## Known limitations (current)

- The corpus funneled-set claim is 12 nodes; the copy holds 11 `funnel_state.tsv` rows. Recorded in `AUDIT.md`; no action needed for W1.
- The corpus atomic-flag claim is 30; the copy holds 29 rows. Same note.
- Owner coordination note (§13 of the goal) must be relayed to the M6 agent by the owner (OA-1).
