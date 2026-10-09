# Architecture & History — the single project document

Everything about how the Onion Knowledge Engine works, why it is shaped this way,
and what was learned building it.

---

## 1. The Onion — layer semantics

| Layer | File | Node | Meaning |
|---|---|---|---|
| L0 | level_00 | Phase | Program phase (P01..P09) |
| L1 | level_01 | Artifact (IN/OUT) | Inputs (green) / outputs (amber), interleaved |
| L2 | level_02 | Task | Work task |
| L3 | level_03 | Sub-task | Sequenced step |
| L4 | level_04 | Detail (TD) | Verb-level activity |
| L5 | level_05 | Item | Work item (50–500 per TD) |
| L6 | level_06 | Sub-item | Nested decomposition |
| L7–L9 | level_07..09 | deeper | Ready for further nesting (hard cap L9) |

- IDs uppercase (`TD1362`, `TK0012`); path = parent path + `-` + own ID
  (`P01-IN01-T03-ST47-TD1362-TK15001-TK16400`). Renames never break links.
- Row placement: level_05 rows must have a TD parent; level_06 an L5 parent; etc.
- TD count per sub-task is **dynamic** — derived from "what distinct activities
  must be done" — never a fixed 4. Questions are a generation method only;
  question text is never stored in CSVs/JSON/HTML.
- Core coherence rule: a TD's items must BE the object of its verb
  (Map processes → process names; Measure cycle time → metrics;
  Classify system type → type categories). Wrong-domain vocab = redo.

## 2. The three loops

### ingest — lexical trust
- canonicalize: case-fold → punctuation→space → singularize (ies→y, −s/−es,
  keep ss/us/is/ses/ches/shes) → drop filler → sort tokens
- similarity = max(LCS ratio, Jaccard) on canonical keys
- ≥0.95 same-TD → skip; ≥0.85 → auto-merge / reuse depot spelling;
  0.70–0.85 → add + flag; else new
- style gates: Title Case, no `?!`, no emoji, ≤8 words, no trailing punct
- input: TSV `td<TAB>title[<TAB>sub1|sub2|…]`; idempotent

### llm — validation sweep
- walks every node L0→L9; one stateless call per node → `{valid, fixed,
  admits_children}`; applies title fixes; resumable via node_llm_report.tsv

### funnel — four-agent generation
Per node: **Planner** (4–8 aspects) → parallel **Workers** (top-100 per aspect,
merged ≤500) → trims (best 400 → 300) → **Critique** (score 0–1, remove/add,
issues) → **Synthesizer** (final 3–100 items, each `atomic` or nest further).
Non-atomic children re-enter the queue — *nest and nest until the simplest
item possible*.

Self-healing:
- critique `add` items flow into the synthesizer (miss fixed same pass)
- score < 0.7 or API failure → node re-queued once with its issues injected
  into the Planner (the next loop fixes the last one's misses)
- children written + state appended per node → crash-safe, resumable;
  seeds descend PAST finalized nodes into unfinished descendants

Model: OpenRouter default `google/gemini-2.5-flash-lite`, `reasoning:{effort:"low"}`, temp 0, max_tokens 6000,
curl `--max-time 120`, stateless (no memory, no conversation). For images there
endpoint is OpenRouter (`DEPOT_API_URL` overrides; legacy DeepSeek worked with
image input) — used by `vision.py` to extract knowledge items from diagrams. Wire format:
`content` is an escaped JSON string (`\n`, `\"`) — parsers unescape fully.

## 3. Threading & supervisor

- `depot_io.cpp`: live-resizable worker pool — mutex + condition_variable +
  atomics; manager reads `control/concurrency.txt`, spawns/retires workers
  between nodes (in-flight nodes finish); `THREADS` lines logged
- `depot_core::policy_target` (pure, unit-tested, WASM-exportable):
  base = clamp(cores,2,16); idle (load<0.3×cores) +2; busy (load>0.8×cores) −2;
  RAM<1GB → /2; NIC>70% → cap 4; GPU>90% → cap 2; clamp 1..5000
- `controller.py` supervisor: samples CPU/RAM/NIC/GPU, hysteresis ≥30s,
  owns the funnel process (restart-on-crash, clean rc=0 → idle), pause/resume,
  thread override, state.json, HTTP API on :8080
- `dashboard.html`: WebGL 3D onion + gauges + controls; policy engine = WASM
  (`depot_core_wasm.js`, built when emcc exists) with identical JS fallback

## 4. Verification gates

1. dup TD/TK ids = 0; orphans = 0 (every L6 parent exists in L5, etc.)
2. every TD has 50–500 L5 items
3. spot-check TD-verb vs first items
4. browser: no empty expanders, chips correct, +18px indent per level
5. `cmake --build build && ctest --test-dir build` — 92/92 (incl. hermetic
   mocked-LLM tests, real-wire-format regressions, pool-resize test)

## 5. Key decisions & history (condensed)

- IDs uppercase; L5 rendered as direct nodes with layer chips; each layer a color
- Questions never stored; TD counts dynamic; 50–500 items per TD
- IN01 rebuilt task-by-task: T01 43 TDs, T02 51, T03–T09 164 (was fixed 4s);
  258 TDs total; every TD ≥50 items
- L6 expansion: every L5 title classified once (16 subagent batches, 5,739
  titles → 1,332 expandable, 18.6k L6 rows); decision ledger expand_decisions.csv
- Review passes fixed: real-wire escaped-content parsing, curl exit-code
  checks, self-reference guard (node ≠ its own child), --dry writes nothing,
  header corruption, TSV sanitization, strict --levels parsing
- provider history: started on DeepSeek (`reasoning_effort:"none"` after pro's reasoning ate
  max_tokens → empty content → `reasoning_effort:"none"` fixed it
- Funnel round-count bug (double-seed) fixed; retry cap 1 per node
- Supervisor lessons: keep-alive must not restart after clean rc=0;
  link speed −1 (unknown) must mean "no link"

## 6. NEVER REMOVE (learned the hard way)

- `depot_core` exported functions (all 21 referenced by code or tests)
- resume-state TSVs (`funnel_state.tsv`, `node_llm_report.tsv`) and the live
  `funnel_run.log` (dashboard/controller parse them)
- `expand_decisions.csv` (ingest + funnel dedupe)
- The 92-test suite — it encodes every bug we fixed

## 7. Security review fixes (multi-agent audit)

- API key removed from source (env `OPENROUTER_API_KEY`, legacy `DEEPSEEK_API_KEY`)
- HTTP surface hardened: static whitelist (main.cpp/binary no longer servable),
  path-boundary via basename whitelist, default bind 127.0.0.1 (`--bind 0.0.0.0`
  opt-in), POST body clamp 4KB, atomic state.json writes, XSS-escaped policy log
- Concurrency: done-set read now under the mutex; retry-exhausted/failed nodes
  are settled (done) so `--rounds N` never re-runs them (regression-tested);
  run_llm error rows aligned to 9 columns; done_ct single-increment
- Ops: per-run counters (since last RUN_START), loopback excluded from NIC
  sampling, pending-expansion dedupe, `--dry` writes nothing, legacy Python
  loops removed (single canonical C++ implementation)

## GPU compute (OpenCL) and local-LLM path (Track A)

Decision (2026-10): the project gains an optional OpenCL compute layer for
device access, scoped acceleration and GPU telemetry; **LLM inference is
delegated to llama.cpp** (OpenCL backend preferred; Vulkan/SYCL measured as
fallbacks) rather than hand-written OpenCL transformer kernels.

- `depot_gpu.{hpp,cpp}` + `data_depot gpu [--info|--json|--selftest]` —
  RAII-free C API wrapped cleanly; no OpenCL types leak into `depot_core`.
  Headers vendored in `third_party/OpenCL-Headers`; the loader is linked from
  `libOpenCL.so.1` when present, otherwise the target (and command) auto-skip
  exactly like `onion_gl`.
- Multi-device (DONE): every fp64 device gets its own engine; calibrated
  proportional split; concurrent enqueue-then-read. Funnel nodes run a local
  near-duplicate sweep (sim >= 0.95) before the LLM trimmer.
- First consumer kernel (Phase 2, DONE): exact `sim_keys` parity on the GPU
  for `ingest --gpu` (accelerates the global-spelling search over ~75k
  candidates; CPU remains the default and the parity oracle). Results on
  POCL (12-thread CPU OpenCL): bench 513k pairs/s vs 194k CPU (2.65x);
  end-to-end 40-row ingest 2.9s vs 7.3s (2.5x), byte-identical dry-run
  reports. Kernel cap 256 chars, unsorted/over-long pairs fall back to the
  CPU transparently; requires `-cl-std=CL3.0` (POCL) and copies strings to
  aligned private memory (POCL miscompiles dual unaligned global streams).
- Telemetry (Phase 3, DONE): the controller samples GPU load from
  nvidia-smi -> intel_gpu_top (one JSON sample) -> Intel i915 sysfs
  frequency ratio (idle ~23% at 300/1300 MHz, ramps to ~100% under load),
  and `/api/state` now carries an `opencl` block (device list from
  `data_depot gpu --json`, cached 10 min). The existing policy cap at
  GPU > 90% applies unchanged.
- Local LLM (Phase 4): `llama-server` subprocess first, connected through the
  existing `AskFn` transport seam; hybrid provider mode keeps the DeepSeek
  API for long-context funnel stages.
- GPU runtime on this machine: POCL (CPU) is installed; install Intel's
  runtime for the Iris Xe with `sudo apt install intel-opencl-icd` (requires
  an interactive sudo; not available passwordless).

