# Complete Reference — CLI, env, HTTP API, control files

## 1. `data_depot` CLI (`build/data_depot <cmd> [options]`)

### ingest
```
data_depot ingest research.tsv [--dry] [--gpu] [--restrict PREFIX] [--root DIR]
```
Input TSV: `td<TAB>title[<TAB>sub1|sub2|…]`. `--gpu` accelerates the ~75k-candidate spelling search (CPU remains the parity oracle). `--dry` produces identical output without writes.

### llm
```
data_depot llm [--levels 0,1,2,3,4,5,6,7,8,9] [--limit N] [--offset N]
               [--concurrency N] [--dry] [--force] [--root DIR] [--model M]
```
Stateless root-to-leaf validation sweep: one call per node → `{valid, fixed, admits_children}`; resume/done-set = first column of `node_llm_report.tsv`; `--force` re-checks done nodes. Defaults: all levels, limit 0 (all), offset 0, concurrency 8. `--levels` is strict — invalid tokens are an error, not level 0.

### funnel
```
data_depot funnel [--branch P01-IN01-] [--root-node PATH] [--limit 50] [--rounds 1]
                  [--concurrency 8] [--dry] [--force] [--all] [--deep]
                  [--model M] [--root DIR]
```
See `docs/FUNNEL_ENGINE.md` for full semantics.

### gpu
```
data_depot gpu --info | --json | --selftest | --bench similarity [--k 64 --n 4096]
```

## 2. Environment variables

| Var | Meaning | Default |
|---|---|---|
| `OPENROUTER_API_KEY` | preferred API key | — |
| `DEEPSEEK_API_KEY` | legacy fallback key | — |
| `DEPOT_API_URL` | endpoint override (any OpenAI-compatible chat/completions API) | key-routed: `sk-or-*` → OpenRouter, else DeepSeek |
| `DEPOT_CONTROL_INTERVAL_MS` | funnel pool reconcile cadence | 3000 (min 50) |
| `DEPOT_DUMP_BODIES` | dump every payload+response to `/tmp/depot_body_NNN.json` | unset |
| `ONION_CPU_THREADS` | CPU-share threads in the GPU engine (0 = off) | hardware concurrency |
| `ONION_GPU_ONLY` | pin one OpenCL device (index) | — |
| `ONION_GPU_DEBUG` | split/timing/profiling logs | unset |
| `ONION_ROOT` | root dir for `rebuild_json.py`/`gen_onion_html.py` | script dir |
| `ONION_PORT` | dashboard port (onion-start default) | 8080 |
| `ONION_BIND` | bind address | 127.0.0.1 |
| `ONION_LIMIT` | loop budget default | 10 |
| `ONION_THREADS` | pin worker count (1–5000) | adaptive |
| `ONION_MODEL` | model id | key-derived |
| `ONION_TUI_URL` | tui / onion_gl state URL | `http://127.0.0.1:8080/api/state` |

## 3. `controller.py` flags

`--root DIR` · `--seed PATH` · `--branch BRANCH` · `--limit 50` · `--rounds 1` · `--deep` · `--force` · `--model M` (default key-derived) · `--initial 8` · `--threads N` (pin; 0 = adaptive) · `--interval 2` · `--hysteresis 30` · `--port 8080` · `--bind 127.0.0.1` · `--no-server` · `--dry` (sample+policy only).

## 4. HTTP endpoints (controller)

| Method | Path | Body / effect |
|---|---|---|
| GET | `/api/state` | full state JSON |
| GET | `/api/control` | `{paused, override}` |
| POST | `/api/pause` | writes `control/paused`, removes `stopped` |
| POST | `/api/stop` | writes `control/stopped`, removes `paused` |
| POST | `/api/resume` | removes both, writes `control/start.txt` |
| POST | `/api/threads` | `{"max": N}` — 1..5000 → `control/override.txt` (cap); 0/invalid → remove |
| GET | `/` | `dashboard.html` |

Static whitelist: `dashboard.html`, `depot_core_wasm.js`, `three.module.js`, `three-addons/*.js`, `workflow_showcase.html`, `progress.html`, `favicon.ico`, `Onion_Review.html`. Everything else 403/404. POST body ≤ 4096 B. `Cache-Control: no-store`. Default bind is loopback — use `--bind 0.0.0.0` (and a firewall/tunnel) only when needed; the API has **no auth**.

## 5. Control files (`control/`)

| File | Written by | Read by | Meaning |
|---|---|---|---|
| `concurrency.txt` | controller | funnel | target worker count (1–5000) |
| `paused` | API/pause | controller | funnel held down |
| `stopped` | API/stop | controller | funnel stopped (until `/api/resume` or `start.txt`) |
| `start.txt` | API/resume | controller | one-shot manual start |
| `override.txt` | API/threads | controller | thread cap (cleared on controller startup) |

## 6. Helper scripts

- `./onion-start <node|branch> [--limit N] [--port N] [--bind 0.0.0.0] [--threads N] [--model M] [--deep] [--force] [any --flag=value for controller]` — refuses if a controller already runs; raises `ulimit -n` to 65535; trailing-dash target → `--branch`, else `--seed`.
- `./onion-stop` — anchored pgrep patterns, SIGTERM then SIGKILL.
- `./onion-tui` — textual dashboard (auto-spawns controller).
- `./onion-gl` / `./onion-gl --term` — native OpenGL / terminal (sixel or half-block) dashboard.

## 7. Build & test

```
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build          # 92 tests (84 core + 8 GPU), hermetic (mocked LLMs), no network
```
Optional: OpenCL layer (`depot_gpu*`, auto-skip without loader), `onion_gl` (auto-skip without X11/GL), WASM `depot_core_wasm.js` (when `em++` exists — copy into the project root to activate the dashboard's WASM policy engine). GoogleTest v1.15.2 fetched at configure time.
