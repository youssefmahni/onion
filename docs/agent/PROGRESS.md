# Progress

## Current

- Milestone: M3 · branch m3-engine · started 2026-10-10
- M2 merged (`24a90eb`), tagged `m2-rc`
- Tests: 182/182 new-tree on Linux (gtest + python) · legacy 98/98 · CI: pending push

## Log (newest first)

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

## Background processes started

- none (the fake servers and node runs used for M3 exit evidence were stopped)
