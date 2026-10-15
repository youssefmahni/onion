# D1 handoff — tree endpoints, viewcache, screens 5–7

Status: **implemented, tests green locally, not yet PR'd.** Branch
`dash/d1-tree` (WIP commit, pushed). The §8 performance proof was mid-run at
handoff: `dev-import` of the 316k corpus (`~/onion-opt-data`) into
`$PWD/.build-tmp/corpus-bench` may still be running in the background; wait
for it (`pgrep -f dev-import`) or re-run it before benchmarking.

## What landed (branch dash/d1-tree, on top of merged D0/main)

| Area | What |
|---|---|
| §6.1 counters | `settles.end` column (migration), `stats_levels(layer,nodes)`, `stats_status(status,n)`; per-fold deltas driven by `sqlite3_changes` (idempotent across replay/shuffles); `StateWriter::recompute_stats` at every startup; the fold-idempotency property test dumps both counters |
| §6.2 viewcache | new `src/viewcache` module: `counts(path,children,subtree)` (bottom-up subtree fold), FTS5 `fts` over `fts_titles` (external content, `SQLITE_ENABLE_FTS5` added to `onion_sqlite`), `meta(version, applied_vector)`; `rebuild/stale/node_added/node_removed/counts_for/search`; mutex-serialized; 4/4 tests |
| §4.1 API | `GET /api/v1/tree/children|node|search|stats|export.csv` (keyset cursors, capped 200/page, export chunked ≤1M rows) behind the D0 session/token guard; view mode (no auth, no token) serves open reads; `/api/v1/version` carries `read_only`. Providers live in `src/api/tree_json.{hpp,cpp}` (pure functions over `store::Db` + optional `viewcache::Cache`) |
| CLI | `node run` wires the cache + maintenance thread (StateWriter `on_folded_record` hook → note queue → incremental updates → throttled 30 s rebuilds on vector mismatch); `view --db <path> [--port 8081]` read-only viewer with per-thread ReadPool; `bench api --db <copy> [--requests 50]` printing p50/p95/p99 per endpoint |
| Frontend | `web/lib/vlist.js` (virtual list), `web/screens/{tree,search,stats}.js` (preact+htm), router in `app.js` with read_only nav hiding + `/`-to-search; styles for the tree screens; all embedded at build time |
| Tests | api 27/27 (7 D1 + 2 view-mode), viewcache 4/4, state 34/34 (extended property test), platform 12/12 |

Live smoke already verified via `view --db` + curl: root children (after the
`parent_id IS` fix), node detail with cache counts, search (prefix + path
jump), stats, export.csv.

## Remaining work, in order

1. **§8 perf proof.** `./build/linux-debug/data_depot bench api --db
   "$PWD/.build-tmp/corpus-bench/onion.db"` after the corpus import finishes.
   Budgets: children/node/stats ≤ 30 ms p95; search ≤ 100 ms p95. If a budget
   misses, fix (likely candidates: search without the cache, the stats
   `COUNT(*)` queries — the counters already make those O(1)).
2. **Browser smoke** (chrome-devtools MCP is available): open
   `view --db` on the corpus, check all three screens render with no console
   errors, and scroll a 10k-row children list (e.g. a layer-1 node with many
   children) through the virtual list.
3. **Docs:** append the D1 result to `docs/agent/WORKLOG.md` and update
   `docs/agent/PROGRESS.md`; add the vendored-libs rows for vlist (none —
   vlist is in-repo, no version pin needed).
4. **Commit + PR:** branch is committed as WIP; run the full local
   `ctest --preset linux-debug`, push, open the PR, get 3 green CI runs, merge
   (`gh pr merge --merge`). Remember: main is checked out in the
   `~/onion-opt` worktree — branch from `origin/main` only.
5. **D1 exit criteria to record in the PR body:** §8 budgets measured on the
   316k corpus copy, virtual list smooth at 10k rows, all D1 tests seen
   passing, old `/api/state` untouched.

## Gotchas learned (keep in mind)

- `store::Statement::step()` returns false at SQLITE_DONE — success, not
  failure; check the error string, and don't `!step()`.
- FTS5 shadow tables collide with content tables named `<fts>_data`; the
  content table is `fts_titles`. External-content FTS indexes nothing until
  `INSERT INTO fts(fts) VALUES('rebuild')`; incremental row ops are plain
  `INSERT INTO fts(rowid,title,path) VALUES(...)` and the `'delete'` special
  command with the ORIGINAL column values.
- The cache is one sqlite handle shared by handler threads + the maintenance
  thread: every public Cache method takes `mutex_`.
- Root children queries must use `parent_id IS (SELECT …)` (NULL parents).
- Keyset pagination only: never OFFSET (plan §3.2).

## The continuation prompt for the next agent

```
Work on the Onion repo (rules in AGENTS.md). Dashboard plan:
docs/DASHBOARD_PLAN.md; live handoff: docs/dashboard/D1_HANDOFF.md and the
latest docs/agent/WORKLOG.md entry. D0 is merged (PR #51); D1 is implemented
on branch dash/d1-tree (pushed as WIP, NOT yet PR'd). Continue D1 to its exit
criteria, in order: (1) finish the §8 perf proof — wait for the dev-import
background job (pgrep -f dev-import) or re-run
`./build/linux-debug/data_depot dev-import --root ~/onion-opt-data --data-dir
$PWD/.build-tmp/corpus-bench`, then
`./build/linux-debug/data_depot bench api --db
$PWD/.build-tmp/corpus-bench/onion.db` and verify children/node/stats ≤30ms
p95, search ≤100ms p95 (fix code if a budget misses); (2) browser-smoke the
three tree screens via view --db with chrome-devtools (no console errors,
virtual list smooth on a 10k-row children list); (3) full local
`ctest --preset linux-debug` (expect api 27/27, viewcache 4/4, state 34/34);
(4) commit any fixes, push dash/d1-tree, open the PR against main, get 3
green CI runs, merge with a merge commit. House rules: branch only from
fresh origin/main (main is checked out in the ~/onion-opt worktree — never
touch it), stage explicit paths, no secrets, append WORKLOG.md after the
task with evidence and a Next step: line.
```
