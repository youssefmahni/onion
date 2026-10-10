# Progress

## Current

- Milestone: M6 · branch m6-sync · started 2026-10-10
- M5 merged (`c074d9e`, PR #11), tagged `m5-rc`; M4 merged (`5b03cb4`), tagged `m4-rc`
- Tests: 206/206 new-tree on Linux · legacy 98/98 · CI green on the M5 merge

## Log (newest first)

- 2026-10-10: M5 merged as PR #11 (`c074d9e`), all 8 CI jobs green, tagged `m5-rc` · gate report docs/agent/gates/M5.md · the legacy pool-resize flake was fixed for real: the manager runs work() inline so the old handshake deadlocked (PR #9), and the announce print is torn by concurrent workers (PR #10) — the test now counts atomic announce prefixes · no real calls
- 2026-10-10: M4 merged as PR #8 (`5b03cb4`), all 8 CI jobs green, tagged `m4-rc` · gate report docs/agent/gates/M4.md (signed log, torn-tail at every byte offset, checkpoints, genesis import verifier; corpus-copy import scripted as OA-10) · one legacy flake observed on Linux CI, passed on re-run, no product change · no real calls
- 2026-10-10: M4 in place: device keys and Ed25519 (golden cross-check), full batch verification, zstd-sealed segments, verified torn-tail repair (tested at every byte offset), slot-scoped 64-bit TK ids, checkpoints with signed manifests and Merkle roots (decision 0009), the eviction-signature rule, admin keygen, the genesis import with duplicate proposal/resolution and a CSV verifier, CUTOVER.md and the weekly dry-run script · corpus-copy import is an owner action (OA-2 pending) · no real calls
- 2026-10-10: M3 merged as PR #7 (`1b3eee5`), all 8 CI jobs green, tagged `m3-rc` · gate report docs/agent/gates/M3.md with the offline 409-node campaign, P7 scans, live review accept, cost projection, kill-at-every-commit-step and the real DeepSeek run ($0.0135)
- 2026-10-10: M3 real campaign against DeepSeek: one node, 3 attempts (0.35/0.35/0.22), all below the floor → review, nothing committed; audit records carry real token counts and peak-priced cost ($0.0135) · hard crash found and fixed: the strict parsers threw on a reply whose `issues` field was an array instead of a string — parsers now reject malformed replies without throwing
- 2026-10-10: ATTEMPT audit records now carry the real token counts and the peak-priced cost (CLI --price-in/--price-cache/--price-out; node.conf arrives in M5); the funnel result counts LLM calls exactly
- 2026-10-10: M3 end-to-end offline campaign via the CLI: fake funnel server, dev-import root, `campaign start` with cost projection, `node run` → 409 settled nodes, ended by the stop rule, campaign closed; P7 log scan clean (no failed attempt committed for any settled node); review accept over the live API (token + Origin + Content-Type) moves the item to decided; dashboard served at `/` · no real calls
- 2026-10-10: Found and fixed an M2 client bug: the assistant `content` was extracted with a regex that stopped at the first escaped quote, truncating JSON content (funnel replies are escaped JSON on the wire). Replaced with a nlohmann parse. All M2 client tests stay green.
- 2026-10-10: M3 modules in place: log (records, batches, canonical JSON, HLC, segments, NullSigner), store (DESIGN schema, WAL, subtree query, CSV export), state (single state-writer thread, ATTEMPT group-commit, deterministic fold, replay), gate (prompts, pre-check, deterministic checks, best-of-3, funnel loop, quality guard), sched (campaign spec, ticket ordering, stop rule, RESERVE chunks, budget waiting, review flows), api (token/Origin/Content-Type checks), cli (node/campaign/review/dev-import/export-csv), dashboard, fake funnel mode · decision 0008 · local new-tree 182/182 · no real calls
- 2026-10-10: Note for the owner: port 18081 on this machine is used by your own `deepseek_api_relay.py` (openclaw-relays). While testing the fake server I briefly killed it by mistake; its supervisor restarted it within seconds. I switched my tests to port 18091+.
- 2026-10-10: M3 started from merged M2 (`m2-rc`); engine-level crash tests use forked children with `_exit` at each commit step.

## Real API usage

| Date | Milestone | Purpose | Calls | Cost (USD) |
|---|---|---:|---:|
| 2026-10-10 | M2 | Real-call validation (decision 0004 update): models list, request shape, usage fields, truncation, 401 | 5 (1 free /models, 1 unbilled 401, 4 billed) | $0.0000126 |
| 2026-10-10 | M2 | Real concurrency probe (adaptive limiter start 4 / ceiling 50) | 100 | ≤ $0.0003 (upper-bound estimate; build predated per-run token totals) |
| 2026-10-10 | M3 | Real small campaign: one node ("Order Processing"), light funnel, real DeepSeek — 3 attempts scored 0.35 / 0.35 / 0.22, all below the gate, nothing committed (P7 live), node went to review | ≤ 22 (pre-check + 3 attempts; exact per-run call counting added after this run) | $0.0135 (5,907 input + 9,772 output tokens at peak prices 0.30/1.20) |

## Background processes started

- none (the fake servers and node runs used for M3 exit evidence were stopped)
