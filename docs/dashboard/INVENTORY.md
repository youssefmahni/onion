# Dashboard Inventory — current API, store, read-only access

Written for §2 of `docs/DASHBOARD_PLAN.md` (2026-10-15). Everything below is
observed from the working tree at commit `5d6a2dd` (branch `b3-gate-funnel`).

## 1. Current local API (`src/api/api.cpp`, `src/api/api.hpp`)

Server: httplib v0.18.3 (FetchContent, CMakeLists.txt:177), plain HTTP,
**compiled without OpenSSL/zlib/brotli** (CMakeLists.txt:208–211; linking the
httplib target would pull Homebrew OpenSSL and break the Intel macOS 12 build).
Bind: `host` (default `127.0.0.1`) × `port` (default 8080, 0 = ephemeral via
`bind_to_any_port`). One server, one accept loop (`listen_after_bind`).

| Route | Auth | Notes |
|---|---|---|
| `GET /api/state` | none | `state_json()` provider (commands.cpp:348): running, api_port, sync_rounds_completed, spend_usd, spend_rate_usd_per_hour, vector, merkle_root, campaigns (id/state/reason/reserved/reserved_uncapped/reserved_by_origin/audits/spec), members (device_id/state/slot), reviews (path/reason/state), review_backlog, settled_nodes, forks, claims, steals_active, lane_counts, quality_guard, data-bytes, budgets… |
| `GET /api/tree?path=&depth=&offset=` | none | lazy subtree provider `tree_json()` (commands.cpp:204); depth clamp [0,6]; per-node child cap with `has_more` + `offset`; reads via the per-thread read-only connection |
| `GET /` | none | `dashboard.html` **loaded from disk at runtime** (`ONION_WEB_DIR`), not embedded |
| `POST /api/campaign/pause` `resume` `stop` | token+origin+ct | body `{id, note}` → `{ok:true}` or 409 |
| `POST /api/review/accept` `retry` `reject` | token+origin+ct | body `{id, note}` |
| `POST /api/stop` | token+origin+ct | body `{leave}` → stop_node hook |

**Token mechanism:** per-device random token from the platform key store
(`api-token` key; created on demand, commands.cpp:159 `api_token()`). Sent as
`X-Onion-Token` header or `?token=` query param. The existing dashboard page
stores it in `localStorage("onion-token")` and appends it to fetches.

**Existing POST checks** (`authorized()`, api.cpp:24–61):
1. token present and equal;
2. `Origin` == `http://127.0.0.1:<bound_port>` (403 otherwise);
3. `Content-Type` contains `application/json` (415 otherwise).

**Missing vs §4/§5 of the plan:** no `/api/v1` namespace; no session/cookie
auth (`/auth` flow); no `X-Onion-Request` header; no CSP or security headers;
no SSE stream; no Idempotency-Key; no 202/`op_id` pattern (actions run inline
in the handler thread and answer synchronously); no gzip (httplib built
without zlib); no ETag; no `metrics/*`, `events`, `alerts`, `reviews/{id}`,
`tree/*` (children/node/search/stats/export), `members`, `invites`,
`evictions`, `review-settings`, `device/*` endpoints; no `campaigns` reads.

**CLI commands today** (src/cli/main.cpp:567–577): `llm`, `node`, `campaign`,
`review`, `dev-import`, `export-csv`, `admin`, `genesis-import`.
No `dashboard` or `view` command yet.

## 2. Store (`src/store/store.cpp` kSchema, `onion.db`)

| Table | Columns |
|---|---|
| `nodes` | id, code, path UNIQUE, parent_id, layer, title, origin, hlc, tombstone (+ `nodes_parent` index) |
| `settles` | path PK, kept, atomic, attempts, score, campaign, hlc, origin |
| `attempts` | path, n, score, passed, tokens_in, tokens_cached, tokens_out, cost, campaign, hlc, origin |
| `critiques` | path, score, issues, remove, add_, hlc, origin |
| `reviews` | path PK, reason, state, note, atomic, hlc, origin |
| `campaigns` | id PK, spec, state, hlc, origin |
| `members` | device_id PK, pubkey, slot, state, incarnation, sync_url, swim_url, profile_version, evict_cutoff |
| `profiles` | device_id, version, prev, reason, changed, spec, rules_version, schema, hlc, PK(device_id, version) |
| `applied` | origin PK, seq |
| `batches` | origin, number, hash, PK(origin, number) |
| `schema_version` | version |

**Not in the DB** (in-memory state or files):
- reserves / budgets: in-memory in `StateWriter` (reserved_total etc.);
- claims/leases: in-memory receiver-clock leases (`writer.active_claims`);
- invitations: signed tokens via `src/log/invite.hpp` (no table);
- eviction proposals: JSON files under `<data-dir>/eviction-proposals/` (no table);
- lane shares: computed from membership (static members.json + members table).

**Values the dashboard needs that do not exist yet:**
- `stats_levels(layer, nodes)` / `stats_status(status, n)` fold counters (§6.1) — absent;
- `viewcache.db` (counts/children+subtree, FTS5 over titles, meta) (§6.2) — absent;
- `metrics.db` speed/spend series (§6.3) — absent;
- an event/activity log feed (only `batches` and per-table hlc exist; no append-only activity index);
- per-device OS/name (members table has no OS/name columns; swim has last-seen in memory);
- per-campaign settles-by-reason and per-level progress are computable from `settles` + `nodes` but have no precomputed form.

## 3. Read-only connections (§2.4) — CONFIRMED PRESENT

- `store::Db::open_readonly()` (store.cpp:318): `SQLITE_OPEN_READONLY |
  SQLITE_OPEN_NOMUTEX`, `busy_timeout(1000)`.
- `StateWriter::read_db()` (state.cpp:1103): **one read-only connection per
  handler thread** (thread_local map keyed by data_dir), so WAL readers never
  serialize behind writer transactions; a failed open falls back to the shared
  handle for that call only.
- Used today by the sync server handlers, `/api/state` and `/api/tree`
  (commands.cpp:1389). → §2.4 satisfied; no read pool work needed for D0.
  D1 must keep using `read_db()` for all tree reads.

## 4. Existing dashboard page (`web/dashboard.html`, 484 lines)

Single HTML file (no build, no framework, inline `<script>`): sections
"Knowledge tree" (lazy `/api/tree` client with filter, expand/collapse,
load-more, 15 s poll of expanded subtrees), "Campaigns", "Review queue",
"Node" (running/spend/spend-rate/settled/review-backlog/quality-guard/
data-bytes + stop button), "Lanes". Token via `?token=` or localStorage.
This page is the seed for D0's shell; per D6 it is removed and `/api/state`
deprecated.

## 5. Observations that adjust the plan (§2.5)

1. **gzip (§3.1):** httplib is deliberately built without zlib. Either vendor
   a minimal deflate (zlib static) or drop gzip for D0 and decide in a record.
2. **`/api/state` has no auth today** and is used by the mp tests and the
   dashboard; D0 must keep it working (exit criterion) while new `/api/v1`
   routes get the full §5 treatment.
3. **Origin check** currently compares against the bound port; the session
   flow (`/auth`) and SSE will need the same origin logic plus `X-Onion-Request`.
4. **`web/` is disk-loaded**, not embedded (CMakeLists.txt:385 `ONION_WEB_DIR`).
   §3.1's embed step is a build change, not a rewrite.
5. **No `dashboard` CLI command exists** — §5's `data_depot dashboard` printing
   the auth URL is new work in `node_cmd` (the node loop already serves `/`).
6. **The quality-gate prerequisite is merged** (58945d7 on main, decision
   0024). **The allowance-leaf prerequisite (0025) is NOT merged** — it rides
   on local commit 260093d (branch `b3-gate-funnel`, unpushed). It must merge
   before D0 coding.
