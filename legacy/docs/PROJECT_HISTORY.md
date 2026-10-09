# Project History — decisions, quirks, and the never-remove list

## 1. Decision history (condensed)

- **Identity = path, not title.** Renames never break links; canonicalization is only for dedup.
- **Append-only level CSVs** + `funnel_state.tsv` resume ledger → crash-safe, idempotent re-runs.
- **Four-agent funnel** (planner → workers → dedup → trimmer → critique → synthesizer) chosen over single-shot generation for quality and auditability; every stage logs to `funnel_run.log` and `funnel_critique.tsv`.
- **Bounded fill** (`--limit`/`--rounds`) over unbounded runs; strict slot claiming prevents budget overshoot.
- **Retry cap = 1** per node; settled-low-score nodes are recorded done so later rounds never re-run them (cost control).
- **Key-driven provider routing** (sk-or → OpenRouter / else DeepSeek, env-overridable endpoint) instead of hard-coded provider config.
- **Provider-aware reasoning knob** — `reasoning_effort:"none"` (DeepSeek) vs `reasoning:{effort:"low"}` (OpenRouter) — after empty-content incidents on both sides.
- **Thread caps raised 64 → 200 → 500 → 5000** with `ulimit -n 65535` (one pipe fd per in-flight curl).
- **Adaptive supervisor with hysteresis + cap override + pin modes**; stale override cleared at startup.
- **Textual for the TUI** (chosen over ratatui/bubbletea — see `docs/CLI_DASHBOARD_RESEARCH.md`).
- **Native OpenGL viewer** with sixel/half-block terminal mode for headless boxes.
- **D3 lazy-materialized tree** for the 300k+-node review page (nothing laid out until visible).

## 2. Notable bugs fixed (each with a regression test)

| Bug | Fix |
|---|---|
| DeepSeek reasoning burns max_tokens → empty content | `reasoning_effort:"none"`; later provider-aware knob |
| OpenAI-style `reasoning` object ignored by DeepSeek → empty-content storm at scale | provider-aware `reasoning_param(key)` |
| Wire-escaped `content` not fully unescaped | `content_json` full unescape, pretty-print tolerant |
| `"true"` inside a `fixed` string flipped parse flags | field-scoped bool parsing |
| `--levels 4a` silently mapped to level 0 | strict parser, invalid → error |
| Funnel double-seed per round | round seeding fixed |
| `--force` queued only the seed, never children | force-mode DFS descends the whole subtree |
| Round-count test race / done-set read unsynchronized | mutex-protected seeding |
| Budget overshoot by in-flight work | claim slot before starting node |
| POCL dual unaligned stream miscompile | aligned private copies |
| Apple serializes kernels across devices | single CL device + native CPU threads |
| Link speed `-1` treated as "saturated" | unknown → no NIC cap |
| Keep-alive resurrecting clean rc=0 finishes | rc==0 → idle, never restart |
| Header corruption writing malformed CSVs | fixed quoting + TSV sanitization |
| `split_pipe("")` → `[""]` empty titles | empty → empty vector |
| curl HTTP errors masquerading as success | non-zero exit → empty body → retry |
| 1200-dot cap truncating rings | one point per node, no cap |
| `onion_gl --term` dumping raw bytes into piped shells | refuse non-TTY; sixel tmux-wrapped |
| Stale `.o` surviving `cmake --build` on macOS | touch sources (documented in OPS) |

## 3. NEVER REMOVE (learned the hard way)

1. Any of the exported `depot_core` functions (WASM + tests depend on them).
2. `funnel_state.tsv`, `node_llm_report.tsv`, live `funnel_run.log` — resume/audit state.
3. `expand_decisions.csv` — ingest expansion registry (one-time 16-subagent classification).
4. The 92-test suite (84 core + 8 GPU) — it encodes every fixed bug.
5. `third_party/` vendored libs (d3, stb_truetype, json.hpp, OpenCL-Headers).
6. The reasoning-knob split (see LLM_AND_PROVIDERS.md §2) — "unifying" it breaks a provider.

## 4. Known open items

- **Local LLM path (Track A Phase 4):** delegate inference to llama.cpp/llama-server through the `AskFn` seam (hybrid: API for long-context funnel stages, local for cheap loops). `docs/opencode-macpro-ollama.md` covers OpenCode → local Ollama on the Mac Pro.
- **L7–L9 mostly empty** — wired but the current corpus doesn't reach that deep yet (L9 is the hard cap).
- `node_tools.tsv` — a planned tools pass (no producer exists in the code yet); when built, merge it into `Onion.json`.

## 5. Provider history

DeepSeek (`deepseek-flash`, `deepseek-v4-pro`) → OpenRouter (`google/gemini-2.5-flash-lite`, $0.10/M prompt / $0.40/M completion). Both remain usable simultaneously via key routing; the DeepSeek-era key backup lives in `backups/.onion_key.deepseek.bak` (gitignored).
