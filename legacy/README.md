# Onion Knowledge Engine

A self-curating hierarchical knowledge base (the "Onion") that powers AI agents,
with three loops:

1. **ingest** — lexical-trust intake of research data (dedupe / merge / spelling-normalize)
2. **llm** — root-to-leaf validation sweep (L0 → L9), stateless LLM verdicts per node
3. **funnel** — four-agent generation loop (Planner → parallel Workers → 400 → 300 →
   Critique → Synthesizer) that nests a node down to atomic items, self-correcting
   low scores and transport failures

## Documentation

| Doc | Covers |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | the master design document (layers, loops, threading, verification gates, decision history) |
| [`docs/DATA_MODEL.md`](docs/DATA_MODEL.md) | the 10-level hierarchy, CSV/TSV formats, id/path scheme, Onion.json schema, ingest merge semantics, integrity invariants |
| [`docs/FUNNEL_ENGINE.md`](docs/FUNNEL_ENGINE.md) | the four-agent funnel in full detail: seeding, budget, retry/settle semantics, pool supervisor, log protocol, flags |
| [`docs/SUPERVISOR_AND_API.md`](docs/SUPERVISOR_AND_API.md) | controller.py: sampling, adaptive thread policy, funnel lifecycle, state.json, HTTP API, security posture |
| [`docs/LLM_AND_PROVIDERS.md`](docs/LLM_AND_PROVIDERS.md) | key routing, endpoints, payloads, parsers, the provider-specific reasoning knob, vision.py |
| [`docs/GPU_LAYER.md`](docs/GPU_LAYER.md) | OpenCL similarity engine, CLI, env vars, performance, quirks |
| [`docs/UI_GUIDE.md`](docs/UI_GUIDE.md) | dashboard.html, tui.py, onion_gl (+sixel), Onion_Review.html |
| [`docs/REFERENCE.md`](docs/REFERENCE.md) | complete CLI/env/HTTP/control-file reference |
| [`docs/OPERATIONS.md`](docs/OPERATIONS.md) | deployment, runbook, run patterns, macOS quirks, troubleshooting |
| [`docs/TESTING.md`](docs/TESTING.md) | test suites, hermetic design, regressions, coverage |
| [`docs/PROJECT_HISTORY.md`](docs/PROJECT_HISTORY.md) | decision history, fixed-bug ledger, never-remove list, open items |
| [`docs/REVIEW_LEDGER.md`](docs/REVIEW_LEDGER.md) | 100-review fleet (10 personas × 10 passes): findings, fixes, open items |

## Quick start (VPS from zero)

```bash
# 1) system packages
sudo apt update && sudo apt install -y build-essential cmake curl python3 python3-venv

# 2) build + test (GoogleTest is fetched at configure time; needs outbound HTTPS)
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build            # 92 tests (84 core + 8 GPU), no network needed

# 3) API key (required for llm/funnel/vision)
export OPENROUTER_API_KEY=sk-or-...  # add to ~/.bashrc to persist
# legacy: DEEPSEEK_API_KEY still works as a fallback

# 4) generate knowledge with the supervisor + web dashboard on :8080
python3 controller.py --seed P01-IN03-T30 --limit 50
#   open  http://<vps-ip>:8080/dashboard.html   (see "Remote access" below)

# 5) optional: terminal dashboard (needs a terminal — run over SSH)
python3 -m venv .venv && .venv/bin/pip install textual
.venv/bin/python tui.py
```

## Shortcuts: onion-start / onion-stop / onion-tui

Three helper scripts live in the project root (works locally and on the VPS):

```bash
./onion-start P01-IN03-T30            # start the supervisor on a NODE (10 loops)
./onion-start P01-IN01-               # or a BRANCH (trailing dash)
./onion-start P01-IN01- --limit 50    # custom budget; any controller flag passes through
./onion-start P01-IN03-T30 --threads 16   # PIN worker count (skip adaptive policy)
./onion-stop                          # stop supervisor + funnel (resumes next start)
./onion-tui                           # terminal dashboard for the running supervisor
```

Environment defaults: `ONION_PORT` (dashboard port), `ONION_BIND` (e.g. `0.0.0.0` to
expose on the LAN), `ONION_LIMIT` (default 10), `ONION_THREADS` (pin threads; empty
= adaptive). `--threads N`/`ONION_THREADS` pins the worker count (1-5000) and the
supervisor stops adapting to machine load. The API key comes from
`OPENROUTER_API_KEY` (legacy: `DEEPSEEK_API_KEY`) or a `.onion_key` file in
the project root (gitignored).

Suggested shell aliases:

```bash
# ~/.bashrc or ~/.zshrc
alias onion-start='$HOME/PlanningProcess/onion-start'
alias onion-stop='$HOME/PlanningProcess/onion-stop'
alias onion-tui='$HOME/PlanningProcess/onion-tui'
export ONION_PORT=8090        # if 8080 is taken
export ONION_BIND=0.0.0.0     # if the dashboard must be reachable remotely
```

## GPU similarity (OpenCL, optional)

`data_depot gpu [--info|--json|--selftest|--bench similarity [--k K --n N]]`
enumerates devices and runs an exact-parity copy of `sim_keys` (headers
vendored in `third_party/OpenCL-Headers`; auto-skipped when no loader).

**CPU + GPU together.** The similarity engine uses the fastest OpenCL
device (a real GPU when present) **and ordinary CPU threads at the same
time**, splitting work in proportion to measured speed; results are
byte-identical to the pure-CPU path. Runtime controls: `ONION_CPU_THREADS`
(default = all hardware threads; 0 disables the CPU share) and
`ONION_GPU_ONLY` (pin a specific OpenCL device). `ONION_GPU_DEBUG=1` prints
the split and per-engine timings. Notes from real measurements:

- Mac Pro: FirePro D500 does ~30x a CPU thread; the CPU share adds only a
  little because the 2013 Xeon is much slower per comparison. Result: no
  regression, small extra (GPU ~5.1M pairs/s, CPU+GPU ~5.2M).
- Apple's OpenCL runtime executes kernels from ONE process one device at a
  time (verified via profiling), so only the fastest OpenCL device is used
  there; on other runtimes additional devices join the same split.
- Linux/POCL: CPU OpenCL device + native threads together give ~5.5x over
  a single CPU thread (Dell: 0.93M vs 0.17M pairs/s).

The funnel also runs a local **near-duplicate sweep** before its LLM
trimmer: candidates with sim >= 0.95 are dropped (GPU/CPU accelerated, logs
`DEDUP removed N` only when something is removed), so the AI trims a
cleaner pool. `data_depot ingest file.tsv --gpu` accelerates the
global-spelling search the same way; the CPU path stays the default.

## Single read artifact — Onion.json

The 10 `Onion - level_*.csv` files remain the storage engine (append-only,
crash-safe); `Onion.json` is the canonical **read** artifact — regenerated by
the controller every ~60 s, or manually via `python3 rebuild_json.py`
(honours `ONION_ROOT`; writes atomically, compact JSON, ~54 MB at 317k nodes).

- `summary` — `generated`, `total_nodes`, `by_layer {L0..L9}`, `funneled`,
  `kept_total`, `atomic_total`, `source_files`
- `levels` — per level: `file`, `columns`, `count` (no row duplication)
- `tree` — nested Phase -> Artifact -> Task -> Sub-task -> Task Detail -> Item
  (items self-nest); each node: `id`, `path`, `name`, `level`, `children`,
  plus `funnel` (`kept/atomic/retries/score/ts`) and `critique`
  (`score/issues/remove/add`) where the node was processed by the funnel

```bash
python3 -c "import json; d=json.load(open('Onion.json')); print(d['summary']['by_layer']); \
  print(d['tree']['P01']['artifacts']['P01-IN03']['tasks']['P01-IN03-T30']['children'])"
```

## Native OpenGL TUI (`onion-gl`)

A C++17 + OpenGL (Xlib/GLX) desktop view of the same `/api/state` — the terminal
TUI rendered as a native GPU window. Runs on any machine with a display; point it
at the VPS when the controller lives there.

```bash
cmake --build build -j          # builds build/onion_gl when X11+GL headers exist
./onion-gl                      # graphical window, local controller (:8080 or $ONION_PORT)
./onion-gl --url http://10.8.0.72:8090/api/state
./onion-gl --term               # in-terminal mode: renders via OpenGL offscreen (EGL)
                                # and paints the frame with sixel (foot, wezterm, xterm)
./onion-gl --term=blocks        # universal fallback: truecolor half-blocks
./build/onion_gl --selftest     # headless geometry + JSON check
```

Terminal mode never opens a window: the UI is rendered into an offscreen GL
pbuffer (EGL, no X needed), read back and emitted as sixel graphics. It
auto-detects sixel (DA1 attribute 4) and silently falls back to half-blocks on
terminals without it; it refuses to run when stdout is not a real TTY (no more
raw-byte dumps when piped or run through an agent), and wraps sixel in tmux
passthrough when inside tmux. Keys work
in raw mode (`q p s t 0-9 c +/- [ ]`), resize is handled (`SIGWINCH`), and the
terminal is fully restored on exit. `--term-size COLSxROWS` overrides the size
probe; `--term-scale F` (or `--scale F`, 0.3-1.0) shrinks the image; `--term-once` prints a
single frame and exits (debugging). The image is sized from the terminal's
real text-area pixels (CSI 14t) less a one-row margin, so it always fits.

Keys: `q` quit · `p` pause · `s` start/continue · `t` stop · `0-9` select ring ·
`c` clear · `+/-` zoom · `[ ]` thread cap (1/4/8/…/5000) · mouse click selects a
ring, wheel zooms. The target is skipped automatically on machines without
X11/GL (e.g. the headless VPS build), so the rest of the project is unaffected.
Vendored single headers: `third_party/stb_truetype.h`, `third_party/json.hpp`.

## Requirements (VPS)

```bash
sudo apt update
sudo apt install -y build-essential cmake curl python3 python3-venv
```

GoogleTest is downloaded automatically at configure time (needs outbound HTTPS to
GitHub). No other dependencies — the core is C++17 stdlib-only; the LLM calls use
`curl`; the dashboard/tools use Python 3 stdlib. (Optional: Emscripten for the
WASM policy module.)

## Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Run the tests (83 cases, incl. hermetic mocked-LLM tests — no network needed):

```bash
ctest --test-dir build
```

## API key

The program reads `OPENROUTER_API_KEY` from the environment (preferred; legacy
fallback `DEEPSEEK_API_KEY`) or the placeholder inside `main.cpp`. Never commit
the real key.

```bash
export OPENROUTER_API_KEY=sk-or-...
```

Traffic routes by key: `sk-or-*` keys go to OpenRouter (`https://openrouter.ai/api/v1/chat/completions`),
anything else to DeepSeek (`https://api.deepseek.com/chat/completions`); override
with `DEPOT_API_URL` to point at any OpenAI-compatible API. The default model is
`google/gemini-2.5-flash-lite` ($0.10/M prompt, $0.40/M completion); pass
`--model <openrouter-model-id>` to use anything else in the catalog. Payloads
send `reasoning: {effort: "low"}` (OpenAI-style) instead of DeepSeek's
`reasoning_effort`.

**Vision:** `google/gemini-2.5-flash-lite` is vision-capable (standard
`image_url` content parts); `vision.py` uses it to answer questions about
images and extract knowledge items ready for ingest:

```bash
python3 vision.py ask photo.png "What process steps are shown?"
python3 vision.py items diagram.png --td "Map current processes" --out items.tsv
./build/data_depot ingest items.tsv
```


## Usage

```bash
# generate knowledge: funnel node P01-IN03-T30 down to atoms, 10 loops
./build/data_depot funnel --root-node P01-IN03-T30 --limit 10 --concurrency 8

# whole branch, repeat 10 verification rounds
./build/data_depot funnel --branch P01-IN01- --rounds 10 --limit 100 --concurrency 8

# validate every node in the tree once (resumable via node_llm_report.tsv)
./build/data_depot llm --levels 0,1,2,3,4,5,6,7,8,9 --concurrency 8

# ingest research (TSV: td<TAB>title[<TAB>sub1|sub2|...])
./build/data_depot ingest research.tsv --dry
./build/data_depot ingest research.tsv --restrict P01-IN01-
```

Common flags: `--limit N` (loop budget), `--concurrency N`, `--dry`, `--force`,
`--rounds N`, `--all`, `--root DIR`, `--model NAME`.

## Going deep: the `--deep` funnel mode

The funnel is breadth-first by default: to add one level it must first finish
every node on the current level, so a wide frontier (dozens of children per
node) makes depth expensive and slow. `--deep` changes two things:

- new children go to the **front** of the queue, so chains descend depth-first
  (the first chain reaches the L9 cap — a 10-segment node never expands — in
  ~8-9 loops)
- every node is **settled on its first pass** (recorded done with its score,
  no retry), so each loop advances the frontier instead of redoing shallow
  work; re-runs strictly resume from where the previous one stopped

```bash
onion-start P01-IN05 --limit 100 --deep     # node form; 100 loops = 100 new nodes
```

Depth still stops where the model marks items `atomic` (cannot be decomposed)
— those are leaves by definition; and filling a whole subtree to L9 remains
exponential, `--deep` just makes progress visible and resumable.

## Adaptive supervisor + dashboard (WASM / WebGL)

`controller.py` owns the machine: it samples CPU/RAM/NIC/GPU every few seconds,
computes a suitable worker-thread count (grow when idle, shrink when the user
is busy, protect RAM/NIC/GPU), writes `control/concurrency.txt`, and the C++
funnel live-resizes its worker pool to match. It also owns the funnel lifecycle
(start / stop / restart-on-crash, clean-finish idle, pause/resume).

```bash
# supervise a funnel run (auto-starts it, adapts threads, serves dashboard :8080)
python3 controller.py --seed P01-IN03-T30 --limit 50
# dashboard:  http://<machine>:8080/dashboard.html
#             3D onion (WebGL) + gauges + controls (pause / thread cap override)
# policy math: WASM (depot_core) when built via Emscripten, JS fallback otherwise
# control files: control/concurrency.txt (threads), control/paused, control/override.txt
# state:        state.json (polled by the dashboard every 2s)
```

The dashboard (`dashboard.html`) is a holographic "mission control" built on
three.js r170 (vendored): 10 neon Saturn rings, each rendered as exactly one
point per node in that layer (316k+ live points, no cap), starfield, bloom
postprocessing, drag-to-rotate, hover + click picking
with a detail card that flies to the selected ring, a left column of normal
cards (Mouse-parallax 3D tilt effect) with live gauges, funnel context
(root/node/step/loop/queued/completion), agent totals, per-node progress, and
pause/thread-cap controls. Vendored assets: `three.module.js` and
`three-addons/` (bloom chain) — keep them next to `dashboard.html`; if missing,
the scene is skipped and the cards still work.
The WASM policy engine is **active**: the dashboard runs the same C++ policy
code (`depot_core.cpp`) compiled to WebAssembly — `depot_core_wasm.js` sits
next to `dashboard.html` (built with Emscripten via
`cmake --build build --target depot_core_wasm`, then copied to the project
root; the JS implementation remains only as a fallback if the file is
missing). The dashboard shows `policy engine: WASM (depot_core)` when active;
the wasm output is identical to the JS fallback (verified on 2000 vectors).
Note for Emscripten ≥ 4: use `em++` (the build target already does).

The old `progress.html` node-by-node view has been retired — its data (per-node
agent progress, agent totals, completion, queue depth) now lives inside the
dashboard itself (NODES card + FUNNEL completion bar), served through
`/api/state`. `Onion_Review.html` is rebuilt every ~60s by the controller while
it runs: a **D3 collapsible tidy tree** (observablehq.com/@d3/tree/2 style,
d3 vendored in `third_party/d3.v7.min.js`) over `Onion.json`. Nodes materialize
lazily on expand, fan-out is capped at 2,000 children with a "…N more" node,
hover shows path/level/children/funnel/critique, and Enter in the search box
expands+centers the first match. Click a node to select it (blue ring), then
use **Expand below / Collapse below** (or right-click the node) to expand or
collapse its entire subtree in one action — capped at 20k visible nodes with a
clear message so even 20k+ node subtrees stay interactive.

## Deployment (Mac Pro server)

The primary deployment is the Mac Pro at `mac@192.168.1.63` (64 GB RAM,
6C/12T Xeon, macOS 12.7.6, headless/de-GUI'd - no auto-login, never sleeps):

```bash
pip3 install --user cmake                     # once, puts cmake in ~/Library/Python/3.9/bin
PATH="$HOME/Library/Python/3.9/bin:$PATH" cmake -S . -B build && cmake --build build -j12
python3 -m venv .venv && .venv/bin/pip install textual
ONION_BIND=0.0.0.0 ONION_PORT=8080 ./onion-start P01 --limit 100
```

- OpenCL builds here via the macOS framework: `depot_gpu` finds the two AMD
  FirePro D500s (OpenCL 1.2 / fp64) - the similarity kernel runs ~17x faster
  than CPU on them; `--gpu` ingestion works unchanged
- `onion_gl` auto-skips (no X11/EGL on macOS); the Textual TUI and the
  browser dashboards are the interfaces
- Scripts are bash 3.2-safe (macOS system bash), handle missing `setsid`,
  and the controller creates `control/` on fresh deployments
- Dashboards: `http://192.168.1.63:8080/dashboard.html` and
  `/Onion_Review.html`; `onion-tui --url http://192.168.1.63:8080/api/state`
  from any machine

## Remote access (dashboard on a VPS)

The controller binds **127.0.0.1 by default** (dashboard only reachable from the
VPS itself). Choose one:

```bash
# A) SSH tunnel (recommended — nothing exposed):
ssh -L 8080:127.0.0.1:8080 user@vps
#    then open http://127.0.0.1:8080/dashboard.html locally

# B) expose the port (protect it: the API can pause/stop runs and serve
#    dashboard files; there is no login):
python3 controller.py --seed P01-IN03-T30 --limit 50 --bind 0.0.0.0
sudo ufw allow 8080/tcp          # or restrict to your IP: ufw allow from <you> to any port 8080
```

Only outbound HTTPS to `openrouter.ai` and GitHub (first build) is required;
no other inbound ports are needed.

## Run as a service (systemd)

```ini
# /etc/systemd/system/onion-supervisor.service
[Unit]
Description=Onion Knowledge Engine supervisor
After=network-online.target

[Service]
User=%i
WorkingDirectory=/home/%i/PlanningProcess
Environment=OPENROUTER_API_KEY=sk-or-...
ExecStart=/usr/bin/python3 controller.py --seed P01-IN03-T30 --limit 50
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now onion-supervisor
journalctl -u onion-supervisor -f
```

The controller is crash-safe: `funnel_state.tsv` resumes the run after restarts,
and a clean finish idles instead of restarting.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no API key` | export `OPENROUTER_API_KEY` (or edit the placeholder in `main.cpp`, not recommended) |
| HTTP 400 model-not-found | the `--model` id does not exist on the endpoint; list ids with `curl https://openrouter.ai/api/v1/models` |
| Empty model replies | reasoning models may burn the token budget — the loops already send `reasoning_effort:"none"`; check the key/model with `curl` |
| Dashboard shows "no state.json" | controller not running (or on another port) — start `controller.py` |
| Rings missing in browser | `three.module.js` / `three-addons/` not next to `dashboard.html` |
| `Address already in use` | another controller instance — `--port` flag or stop the old one |
| TUI import error | `python3 -m venv .venv && .venv/bin/pip install textual` |
| CMake fetch fails | first build needs outbound HTTPS to GitHub (GoogleTest) |

## Files

- `main.cpp` — CLI (defaults: model google/gemini-2.5-flash-lite, limit 50, concurrency 8)
- `depot_io.cpp` — ingest + llm + funnel engines (live-resizable pool, resume state,
  critique audit, curl)
- `depot_core.cpp` — pure algorithms: canonicalization, similarity, JSON parsers,
  thread policy
- `controller.py` — machine supervisor (resource sampling, adaptive threads,
  process lifecycle, HTTP API)
- `dashboard.html` — WASM/WebGL supervisor dashboard
- `Onion - level_XX.csv` — the knowledge base (L0 phases … L6 sub-items; L7–L9 ready)
- `expand_decisions.csv` — title → L6 expansion registry used by ingest

## CLI dashboard (terminal TUI)

`tui.py` is the terminal edition of the dashboard (Textual; sparklines are
drawn natively with block characters — no extra deps). It
consumes the same `/api/state` JSON — no changes to the C++ core. It renders
the onion rings in braille, live gauges + sparkline, funnel context, per-node
agent progress, and has pause/stop/start + thread-cap controls.

```bash
python3 -m venv .venv && .venv/bin/pip install textual
.venv/bin/python tui.py                      # auto-spawns the controller
.venv/bin/python tui.py --seed P01-IN03-T30 --limit 10
```

Keys: `q` quit · `p` pause · `s` start/continue · `t` stop · `+/-` zoom ·
`0-9` select layer · `c` clear · mouse-click a ring to inspect the layer.

See `docs/CLI_DASHBOARD_RESEARCH.md` for the framework comparison behind
this choice.

## Resume / safety

- `funnel_state.tsv` — finalized nodes; the same command continues where it stopped
- `funnel_critique.tsv` — audit: per-node critique score, issues, remove/add lists
- `node_llm_report.tsv` — llm-loop resume + verdict audit
- `--dry` never writes; killing a run mid-flight loses only the current node

## Docs

- `docs/ARCHITECTURE.md` — the single project document: layer semantics, the
  three loops, four-agent funnel, threading & supervisor, verification gates,
  condensed decision history, never-remove list
- `workflow_showcase.html` (repo root) — visual walkthrough of the pipeline
