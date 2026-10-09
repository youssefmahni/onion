# Testing — suites, design, how to run

## 1. The suites

| Binary | Source | Tests | Notes |
|---|---|---|---|
| `depot_core_test` | `depot_core_test.cpp` | 84 | parsers, canonicalization, similarity, policy, ingest/funnel/llm integration; **hermetic — mocked LLM transport (`AskFn`), zero network** |
| `depot_gpu_test` | `depot_gpu_test.cpp` | 8 | auto-skip without OpenCL: enumeration, known-answer vadd, broken-kernel failure, sim parity (pairs/matrix/multi-device/boundaries) |

Run: `cmake --build build -j && ctest --test-dir build` (or `--output-on-failure`).

## 2. Design principles

- **AskFn seam** (`depot_io.hpp`): `run_funnel` accepts an injected `std::function<string(const string& payload)>`; tests route prompts to canned JSON (`run_llm` has no seam yet — see PROJECT_HISTORY.md open items) — every fixed bug is encoded as a regression.
- **Fixture** (`FunnelFixture`): a minimal P01 tree (phase → 1 artifact → 1 task → 1 subtask → 1 TD → 1 existing item) in a temp dir per test.
- The C++ core is pure (no I/O) — trivially unit-testable and WASM-compilable; the policy function is tested against its own mirrored variants.

## 3. Key regressions encoded (non-exhaustive)

- `parse_llm` flag-flip guard (`"true"` inside a `fixed` string must not flip `valid/admits`).
- `content_json` unescape of wire-escaped `content` + pretty-printed multi-line bodies.
- Strict `--levels` parser (garbage → error, not level 0).
- Funnel: API-failure retried then settled; low-score retry cap = 1 and settle-on-second-pass; `--dry` writes nothing; self-reference guard; round-count double-seed bug; **force-mode seeding descend** (`ForceReRunsSettledSubtree`); pool resize from `control/concurrency.txt` (50 ms interval).
- `split_pipe("")` empty-title bug.
- GPU: parity of `sim_pair` vs CPU `sim_keys` on random pairs/matrices; boundaries (empty/equal/over-long); multi-device split.

## 4. Adding tests

1. Mock the LLM (return the JSON a real model would — look at existing fakes).
2. Assert on: return code, captured stdout markers (`funneled=`, `errors=`, `NODE_DONE`…), and **file artifacts** (CSV/TSV row counts and contents) — file-level asserts are what caught every real regression.
3. Keep tests hermetic: no network, temp dirs only.

## 5. Coverage

`-DDEPOT_ENABLE_COVERAGE=ON` builds with `--coverage` (gcov); pair with `gcovr` or lcov for reports.
