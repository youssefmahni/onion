# Onion Control Dashboard — Implementation Plan

> **For the OpenCode agent.** Follow the project rules in `AGENTS.md` (git, secrets, money, integrity, anti-loop rules). OpenCode reads `AGENTS.md` from the repo root, and uses `CLAUDE.md` only when no `AGENTS.md` exists. This plan defines *what* to build and *how to prove it*. Where the existing code differs from what this plan assumes, the code is the starting point: adapt the plan, and record every difference in `docs/agent/WORKLOG.md`.

> **For Youssef, before handing this over**
> 1. Check the owner decisions in §0.
> 2. The repo has a `CLAUDE.md` with the project rules. Copy it to `AGENTS.md` at the repo root (`cp CLAUDE.md AGENTS.md`) and commit it, so OpenCode loads the same rules. Keep the two files identical from then on.
> 3. Export each artboard of the mockup canvas as a PNG into `docs/dashboard/mockups/`, named:
>    - `1-control.png`, `2-campaigns.png`, `3-fleet.png`, `4-review.png`;
>    - `5-tree.png`, `6-search.png`, `7-stats.png`.
>
>    The agent cannot open claude.ai links; these images are its visual spec.

---

## 0. Owner decisions

| # | Decision | Value | Notes |
|---|---|---|---|
| D-1 | Frontend stack | **Preact + htm + uPlot, vendored, no build step** | Alternative: a Vite build (adds Node to CI) |
| D-2 | Remote access | **127.0.0.1 only; SSH tunnel for remote use** | Alternative: LAN bind with TLS (needs certificates) |
| D-3 | "Restart service" in the dashboard | **yes** | Confirmation dialog |
| D-4 | "Leave the fleet" in the dashboard | **no, CLI only** | Irreversible for the device |
| D-5 | Metrics retention | **7 days at 30 s resolution** | About 20,000 rows per series |
| D-6 | Fonts | **Vendor IBM Plex Sans, Space Grotesk, JetBrains Mono (OFL), latin subset, woff2** | Fallback: the system font stack |

Record D-1, D-2 and the bind change (§3.3) in one decision record. Claim the number in `docs/decisions/INDEX.md` first.

---

## 1. Goal and scope

One dashboard, served by `data_depot` itself, to **control** a node and the fleet and to **see** the tree and the work. Seven screens, matching the mockups:

| # | Screen | Purpose |
|---|---|---|
| 1 | Control center | Alerts with actions, key numbers, speed and spend charts, campaign and fleet summaries, recent activity |
| 2 | Campaigns | List, details, progress, projection, plan-before-start, pause, resume, raise budget, edit settings, stop |
| 3 | Fleet | Members and their live numbers, lanes, invitations, evictions (propose, sign, reject), this device's actions |
| 4 | Review queue | Items by reason, attempts and proposed children, accept a selection, new attempt, make a leaf, auto-accept settings |
| 5 | Tree: Explorer | Lazy tree, node details, children table, history |
| 6 | Tree: Search | Full-text search with level, status and phase filters; path jump |
| 7 | Tree: Stats | Per-level counts, phases, how nodes ended |

**Out of scope:** multi-user accounts, editing node titles by hand, anything that bypasses the signed log.

**Separate read-only mode:** `data_depot view --db <path>` opens any copy of `onion.db` read-only and serves only screens 5–7, with no control actions.

---

## 2. Before you start (verify, then adapt)

1. **Prerequisite tasks.** Confirm these are merged; if not, they come first:
   - the allowance-leaf fix for imported tickets;
   - the quality-gate change (critic off by default, checks-based gate, stop rule on the verified atomic share).

   The dashboard shows their outcomes, so they must be in place.
2. **Inventory the current local API:** the routes (search `src/api/` for route registrations), the existing dashboard page, the token mechanism, the `/api/state` shape, the Origin and Content-Type checks. Write the inventory in `docs/dashboard/INVENTORY.md`.
3. **Inventory the store:** the tables and columns for nodes, settles, attempts, critiques, campaigns, reserves, members, reviews, claims. Note which values the dashboard needs that don't exist yet.
4. **Read-only connections:** confirm the per-thread read-only SQLite connections from the sync fix exist. If they don't, add a read pool first: every dashboard read uses it, and none waits on the writer.
5. **Plan adjustments:** record any change to this plan caused by 2–4 in `WORKLOG.md` before coding.

Branches: `dash/d0-…` to `dash/d6-…`, one per phase, each from fresh `origin/main`, merged through a PR with all CI jobs green.

---

## 3. Architecture

### 3.1 Serving

- The dashboard is static files under `web/` (see §6), compiled into the binary at build time: a CMake step generates a C++ source holding each file's bytes, its MIME type and a content hash.
- Served at `/` by the node's existing HTTP server:
  - `ETag` = content hash;
  - `Cache-Control: no-cache` (always revalidates; cheap thanks to the ETag);
  - gzip when the client accepts it.
- `data_depot view --db <path> [--port 8081]` starts only the HTTP server with a read-only store (no engine, no sync) and serves screens 5–7.

### 3.2 Reads and writes

- **Reads:** prepared statements on the read-only pool. Never `OFFSET`; always keyset cursors. Path-prefix range scans for subtrees.
- **Writes:**
  - every action becomes the same signed record the CLI would write, submitted to the state-writer queue;
  - the API answers `202 {"op_id": "…"}`, and the result arrives as an `op` event on the stream (§4.2);
  - every write request carries an `Idempotency-Key` header, and a repeated key returns the first result.
- **No new write paths:** the dashboard calls the same internal functions the CLI uses. If a CLI command has no internal function yet, refactor it into one first, with tests.

### 3.3 Binding and access

- The default bind is `127.0.0.1`. Remove or gate the `--host 0.0.0.0` change: if kept, it requires an explicit `--allow-remote-dashboard` flag that prints a warning at startup.
- Remote use goes through an SSH tunnel: `ssh -L 8080:127.0.0.1:8080 <host>`.

---

## 4. API (`/api/v1`)

All endpoints require authentication (§5). JSON in and out, UTF-8, numbers per decision 0003 (money as decimal strings). Lists use `?cursor=&limit=`, with `limit` at most 200 (default 50), and return `{"items": [...], "next_cursor": "…" | null}`.

### 4.1 Reads

| Endpoint | Returns |
|---|---|
| `GET summary` | Requests in flight, limit, answers/s, spend today vs budget, open/settled/atomic/review counts, sync state |
| `GET alerts` | `[{id, severity, title, detail, actions:[{label, method, path, confirm?}]}]` |
| `GET events?since=<seq>` | Activity log: dashboard and CLI actions, campaign state changes, member changes (last 10,000) |
| `GET metrics/speed?range=1h\|24h\|7d` | Requests in flight, limit, answers/s, output tokens/s, p95, errors per window; at most 1,500 points (downsampled on the server) |
| `GET metrics/spend?range=24h\|7d` | Spend per hour, split peak/off-peak, per campaign |
| `GET campaigns`, `GET campaigns/{id}` | Spec, state, budget used, settles by end reason, per-level progress, live projection |
| `GET members` | Per device: id, name, OS, profile, state, lane share, requests in flight and limit, answers/s, spend today and budget, sync lag, last seen |
| `GET invites`, `GET evictions` | Pending invitations and eviction proposals with their signatures |
| `GET reviews?reason=&cursor=` | Queue items with path, title, reason, created |
| `GET reviews/{id}` | Every attempt: proposed children, check results, atomic labels and verification |
| `GET review-settings` | Auto-accept settings |
| `GET tree/children?path=&cursor=&limit=` | `{path, code, title, layer, status, children, subtree}` per child |
| `GET tree/node?path=` | Node, its settle, attempts summary, history, origin device, campaign |
| `GET tree/search?q=&levels=&status=&phase=&cursor=` | Ranked matches. If `q` looks like a path, it returns that node first (the "jump" case) |
| `GET tree/stats` | Per-level and per-status counts, phases, how nodes ended |
| `GET tree/export.csv?path=` | Streamed CSV of the subtree (chunked), capped at 1,000,000 rows |

### 4.2 Live stream

`GET stream` is a Server-Sent Events stream:
- `summary` every 2 s;
- `alert` (added or cleared);
- `event`;
- `op` (`{op_id, status: applied|rejected, error?}`);
- `members` when a member changes state.

The client reconnects with backoff and resumes with `Last-Event-ID`.

### 4.3 Actions (POST, all return 202 with `op_id` unless noted)

| Endpoint | Body | Notes |
|---|---|---|
| `campaigns/plan` | `{spec}` | **200**, writes nothing; returns the projection and a `plan_id` valid for 10 minutes |
| `campaigns` | `{spec, plan_id}` | Rejected without a recent `plan_id` for the same spec: you must see the projection before starting |
| `campaigns/{id}/pause`, `campaigns/{id}/resume` | – | |
| `campaigns/{id}/budget` | `{add_attempts}` or `{new_budget}` | Never lower than what's already reserved |
| `campaigns/{id}/settings` (PATCH) | Only fields the server lists as mutable | |
| `campaigns/{id}/stop` | `{confirm: "<campaign name>"}` | Final; the server checks `confirm` |
| `invites` | `{pubkey, ttl_hours}` | **200** `{token, expires_at}`; the token is shown once |
| `evictions` | `{device_id, reason}` | Proposes |
| `evictions/{id}/sign`, `evictions/{id}/reject` | – | Eviction applies only with a majority, or the admin key (CLI only) |
| `device/llm-key` | `{key}` | Stored in the key store; never echoed, never logged; the response reports only "set" |
| `device/restart-service` | `{confirm: "restart"}` | Only if D-3 = yes |
| `reviews/{id}/accept` | `{attempt, children: [indices]}` | |
| `reviews/{id}/retry` | `{note}` | |
| `reviews/{id}/leaf` | – | |
| `review-settings` (PUT) | `{auto_accept_after_days, reasons}` | |

### 4.4 Alerts (derived on the server, cleared when the condition ends)

- Campaign paused by the churn guard.
- Circuit breaker open (transport, or authentication/balance).
- Budget exhausted (campaign or device).
- Forked batch quarantined.
- A member SUSPECT or DEAD.
- Sync lag above 3 rounds.
- Disk free below 10%.
- Review queue above its threshold.
- Clock skew warning.

Each alert names its actions, for example Resume, Raise budget, Inspect.

---

## 5. Security

1. **Sign-in:**
   - `data_depot dashboard` prints `http://127.0.0.1:<port>/auth?code=<128-bit one-time code>`, valid for 2 minutes;
   - `/auth` swaps the code for a session cookie `onion_session` (HttpOnly, SameSite=Strict, Path=/) and redirects to `/`;
   - sessions are held in memory and expire after 12 hours of inactivity.
2. **Scripts and the CLI** keep using the existing device token (`Authorization: Bearer`). It is never sent to the browser.
3. **Every mutating request needs all of:**
   - a valid session or token;
   - `Origin` equal to the dashboard's own origin;
   - the header `X-Onion-Request: 1`;
   - `Content-Type: application/json`.
4. **Content-Security-Policy:** `default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'`. No inline scripts or handlers.
5. **Irreversible actions need a typed confirmation,** checked on the server, never only in the UI.
6. **Every dashboard action is recorded:** device, time, action, op id. It appears in Recent activity.
7. **Secrets** (API keys, private keys, tokens) are never returned by any endpoint, logged or shown. Only "set / not set" is shown.

---

## 6. Data

### 6.1 Counters in the fold (in `onion.db`, derived and rebuildable)

`stats_levels(layer, nodes)` and `stats_status(status, n)` are updated in the same transaction as each fold. `summary` and `tree/stats` read them instead of counting. Extend the fold-idempotency property test: applying a log twice or shuffled yields identical counters.

### 6.2 `viewcache.db` (sidecar, beside `onion.db`, never inside it)

- **`counts(path PRIMARY KEY, children, subtree)`.**
- **`fts`:** an FTS5 table over titles, with the path stored for display. Prefix queries; bm25 ranking.
- **`meta(version, applied_vector)`.**

How it's maintained:
- Built by a background thread from a read-only connection, with progress shown in `summary`.
- Updated incrementally from the state writer's "batch applied" notifications.
- On a version or vector mismatch: rebuild. It's a cache: deleting it is always safe.
- `view --db` builds it next to the copy when it's absent.

### 6.3 `metrics.db` (local, beside `onion.db`)

- `speed(ts, inflight, limit, answers_ps, out_tokens_ps, p95_ms, errors)` every 30 s.
- `spend(ts_hour, campaign, peak_usd, offpeak_usd)`, computed from attempt records.
- Retention per D-5. Downsampling happens in the query.

---

## 7. Frontend (`web/`)

```
web/
  index.html            static shell, no inline script
  app.js                router, store, stream client
  lib/                  vendored: preact, htm, uplot (pinned, hashes in third_party/VERSIONS.md)
  components/           shell, nav, alerts, charts, vlist (virtual list), dialog, table, tree, kpi
  screens/              control, campaigns, fleet, review, tree, search, stats
  styles.css            tokens from the mockup (colors, type scale, spacing, radii)
  fonts/                woff2 per D-6
```

- **Routes (hash based, so every view has a link):**
  - `#/control`, `#/campaigns/<id>`, `#/fleet`, `#/review/<id>`;
  - `#/tree/<path>`, `#/search?q=`, `#/stats`.

  Back and forward work.
- **Data layer:**
  - one fetch helper with ETag support and error handling;
  - the stream client feeds the store;
  - writes show "pending" until their `op` event arrives.
- **Long lists:**
  - tree, children, search results and the review queue use the virtual list, drawing only visible rows;
  - the tree loads children 50 at a time.
- **Charts:** uPlot, with a fixed number of points per range from the server.
- **Keyboard:**
  - `/` focuses search;
  - arrow keys move through the tree (roving tabindex);
  - in review, `a` accept, `r` new attempt, `l` leaf.
- **Accessibility:** real buttons and links; focus trapped in dialogs; `prefers-reduced-motion` respected; text contrast at least 4.5:1, matching the mockups.
- **Phone width:** the side panels become drawers, tables scroll inside their own box, the toolbar wraps.

---

## 8. Performance budgets

Measured on a copy of the real corpus (316k nodes) and on a synthetic 1-million-node database:

| Measure | Budget |
|---|---|
| `summary`, `tree/children`, `tree/node` (server time, p95) | ≤ 30 ms |
| `tree/search`, first page (p95) | ≤ 100 ms |
| `campaigns/{id}` including the projection (p95) | ≤ 150 ms |
| Any JSON response (except export) | ≤ 50 KB compressed |
| First load: HTML + JS + CSS + fonts | ≤ 300 KB compressed |
| Stream overhead | ≤ 1 KB/s per open dashboard |
| Scrolling a 10,000-row list | No dropped frames (only visible rows drawn) |

Tools:
- `data_depot bench api --db <copy>` prints p50/p95/p99 per endpoint;
- `tools/gen_corpus.py --nodes 1000000` creates the synthetic database for the nightly job.

---

## 9. Tests

- **C++ unit tests:**
  - auth: no session → 401; expired code → 401; cookie flags present;
  - cross-site: wrong `Origin`, missing header or wrong content type → 403;
  - every endpoint's status codes and JSON shape (golden files);
  - typed confirmation required for stop and restart;
  - `plan_id` required for campaign start;
  - secrets never present in any response (a scan over every golden file);
  - counter idempotency (§6.1).
- **Integration (Python, against a node running with the fake LLM):**
  - each action round-trips: submitted → `op` applied → visible in reads;
  - an idempotency-key repeat changes nothing;
  - the stream delivers summary and alerts;
  - `viewcache.db` stays correct after new batches arrive.
- **Browser smoke tests (nightly in CI, Playwright):** each screen loads with no console errors, main actions work against the fake LLM, and an accessibility scan (axe) passes.
- **Performance (nightly):** `bench api` on the synthetic database; fail if a §8 budget is exceeded.
- **Proof for each phase:** the full suite passes, the new tests have been seen failing before the fix where applicable, and 3 green CI runs.

---

## 10. Phases

| Phase | Delivers | Exit criteria |
|---|---|---|
| **D0** Foundation | Inventory (§2), decision record, bind fix, sign-in, cross-site checks, CSP, embedded static files, `/api/v1` skeleton, empty shell with navigation | All §9 security tests pass; old `/api/state` still works |
| **D1** Tree | `tree/*` endpoints, counters (§6.1), `viewcache.db`, screens 5–7, `view --db` | §8 budgets met on the corpus copy; the virtual list is smooth at 10k rows |
| **D2** Control center | `summary`, `alerts`, `events`, `metrics/*`, `metrics.db`, stream, screen 1 | Live numbers update within 3 s; each §4.4 alert raised and cleared in a test |
| **D3** Campaigns | Campaign reads and actions, plan-before-start, screen 2 | Every action round-trips as a signed record; stop requires the typed name |
| **D4** Review | Review reads and actions, auto-accept settings, screen 4 | Accept, retry and leaf each tested end to end |
| **D5** Fleet | Members, invites, evictions, device actions, screen 3 | Invite → join works; a lone eviction is rejected; the key is never echoed |
| **D6** Finish | Phone layouts, keyboard shortcuts, nightly browser and performance jobs, `docs/OPERATOR.md` dashboard section, `DESIGN.md` update | All budgets enforced in CI; the old dashboard removed and `/api/state` deprecated with a note |

**Order:** start D0 after the two prerequisite fixes (§2.1). D0–D3 must land before the 24-hour soak, since the soak is run from this dashboard. D4–D6 may run alongside M9.

---

## 11. Definition of done

- [ ] D0–D6 merged through PRs with green CI; each phase's exit criteria proven in `docs/dashboard/PROGRESS.md` with links.
- [ ] Every §8 budget measured and enforced nightly.
- [ ] Every §9 security test present and passing; no secret in any response.
- [ ] Screens match the mockups in `docs/dashboard/mockups/`. Deviations are listed with reasons.
- [ ] `docs/OPERATOR.md` covers starting the dashboard, the SSH tunnel, sign-in, and each action.
- [ ] The decision record for D-1, D-2 and the bind change is merged; `DESIGN.md` updated.
