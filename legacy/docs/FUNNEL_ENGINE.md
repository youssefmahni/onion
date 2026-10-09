# Funnel Engine — the four-agent generation loop

Entry point: `depot::run_funnel(branch, limit, concurrency, dry, force, model, key, root, raw_ask, seed_path, rounds, all_branches, deep)` in `depot_io.cpp`.

## 1. Startup

1. Load all 10 `Onion - level_NN.csv` files into `by_path` (path → `{layer, path, title}`), build `children_of`.
2. Load `funnel_state.tsv` → `done` set + `state_retries` map.
3. `tk_next` = max `-TK` suffix across all paths + 1 (atomic id counter).

## 2. Seed resolution

- `--root-node PATH` (if it exists) → single seed.
- Otherwise: **all L4 nodes** matching the `--branch` prefix (`--all` clears the filter).
- Seeds are sorted for deterministic order.

## 3. Round seeding & done-set semantics

Per round (default `rounds=1`), for each seed, a DFS walks the current children map:

- Node **done** (in `funnel_state.tsv`) and **not** `--force` → skipped, **descend into its children**.
- First non-done node per chain → queued.

This is how re-runs resume strictly past finalized nodes.

**`--force`** additionally queues done nodes *and* walks into their children — the whole settled subtree re-runs each round. New candidates are still filtered against existing children (canonical title dedup), so only genuinely new items are kept. (Regression: `ForceReRunsSettledSubtree`.)

## 4. Budget

`--limit N` (0 → unlimited). A worker **claims a slot before starting a node** (`funneled + in_flight >= budget` check), so the budget is never overshot by in-flight work.

## 5. The agent loop per node (`work(path)`)

Skips if `path_segments >= 10` (L9 hard cap). Logs `NODE_START\tpath\ttitle`.

| Step | Agent | Calls | Output |
|---|---|---|---|
| 1 | **PLANNER** | 1 | 4–8 aspects (`{"items": […]}`; capped 8; empty → `["General coverage"]`). On retry, prior critique issues are injected. |
| 2 | **WORKERS** | 1 per aspect (parallel via `std::async`) | top-100 candidate children each; merged, canonical-deduped, capped 500 |
| 3 | **DEDUP** | 0 (CPU/GPU local sweep) | drops candidates with `sim_keys ≥ 0.95` vs an earlier kept candidate |
| 4 | **TRIMMER** | 2 | narrows **400 → 300** (each `{"items":[exactly K]}`; failed reply keeps old list) |
| 5 | **CRITIQUE** | 1 | `{score 0–1 usable-ratio, remove[], add[], issues}` — `parse_funnel_c` defaults score to 1.0 when malformed |
| 6 | **SYNTHESIZER** | 1 | final set `{"items":[{"title", "atomic"}]}` (3–100, resized to 100); critique `add` items flow in **same pass** |
| 7 | filters + writes | — | see below |
| 8 | post-pass | — | `NODE_DONE\tpath\tkept\tatomic\tscore\tFINAL|RETRY-QUEUED\tcalls=N`; non-atomic children queued (`push_front` under `--deep`, else `push_back`) |

All agent calls use `funnel_payload` (`temperature 0`, `stream false`, `max_tokens 6000`, provider-aware reasoning knob).

### Writes (under the mutex)

- Drop items whose canonical form equals the node itself (**self-reference guard**) or any existing child.
- Allocate ids from `tk_next` (`fetch_add`), append `TK%05d,path,title` rows to `Onion - level_<segs>.csv` (append-only, crash-safe per node).
- Update `by_path`/`children_of` so later loops see the new nodes.
- Append `funnel_critique.tsv` row; append `atomic_items.tsv` rows when any child is atomic.

## 6. Retry & settle semantics

- **Low-score gate** (`score < 0.7`): node re-queued **once** with `retry_issues = crit.issues`; a second low pass (or API failure) **settles** it — appended to `funnel_state.tsv` with accumulated retries and inserted into `done`, so later rounds never re-run it.
- **`--deep`** skips retries entirely (every node settled first pass); new children go to the queue **front** (depth-first to L9 in ~8–9 loops).
- **API/transport failure** (empty body or body containing `"error"`): `NODE_FAIL` logged, `errors++`, retried once by re-queue; second failure settles with score 0.0. A killed run loses only the current node.
- Only the final pass is recorded in state (exactly one row per settled node).

## 7. Pool supervisor & log protocol

- Manager loop: round seeding when drained & rounds remain & budget not reached (`ROUND_START\tn\tof N`); completion when budget reached-and-drained **or** all rounds drained (funnel exits rc=0 — the controller does **not** restart clean finishes).
- Pool reconcile: reads `control/concurrency.txt` every `DEPOT_CONTROL_INTERVAL_MS` (default 3000, min 50); valid range 1–5000; workers retire gracefully (finish current node).
- Log events (machine-parseable, tab-separated):
  `RUN_START`, `THREADS\ttarget\tlive`, `ROUND_START`, `QUEUE\tlen\tfunneled\tbudget`, `NODE_START`, `AGENT\tpath\tSTAGE\tEVENT`, `NODE_DONE`, `NODE_FAIL`, final `funneled=… kept=… atomic=… calls=… retried=… errors=…`.
- `DEPOT_DUMP_BODIES=1` dumps every payload+response to `/tmp/depot_body_NNN.json`.

## 8. Flags

`--branch P01-IN01-` · `--root-node PATH` · `--limit 50` · `--rounds 1` · `--concurrency 8` · `--dry` (writes nothing) · `--force` · `--all` · `--deep` · `--model M` · `--root DIR`.

## 9. Determinism & safety properties

- Ids allocated under the mutex → no duplicates under any concurrency.
- Append-only level writes → crash-safe.
- Resume = `funnel_state.tsv` → re-runs are idempotent w.r.t. settled nodes.
- Hermetic: an injected `AskFn` (test seam) replaces all transport; 84 core + 8 GPU tests run with no network.
