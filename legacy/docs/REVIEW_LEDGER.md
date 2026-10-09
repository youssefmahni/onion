# Review Ledger — 100-Review Fleet (10 personas × 10 passes)

**Date:** 2026-10-09 · **Scope:** entire project (code + docs) · **Mode:** read-only reviews, then a fix round.
**Method:** 10 independent agents, each with a distinct persona, each performing 10 numbered review passes over its domain (100 review passes total). Findings were severity-ranked, de-duplicated, and the actionable subset applied and verified (92/92 tests green).

## The fleet

| # | Persona | Domain | Passes |
|---|---|---|---|
| 1 | C++ systems architect | layering, concurrency, ownership, error handling, API seams, state, resources, determinism, tech-debt, docs-vs-architecture | 10 |
| 2 | Security engineer (red-team) | secrets, HTTP surface, injection, input validation, path safety, XSS, persistence, supply chain, DoS, docs-vs-posture | 10 |
| 3 | Performance engineer | hot path, scaling, memory, I/O, LLM cost, GPU, telemetry, web UI, scaling math, docs-vs-limits | 10 |
| 4 | QA/test lead | parser coverage, funnel coverage, concurrency tests, ingest, run_llm, negative paths, resume, fixtures, doc accuracy, live run | 10 |
| 5 | SRE | supervision, resilience, deployment, observability, data safety, networking, upgrades, capacity, incident response, docs | 10 |
| 6 | Data engineer | referential integrity, id invariants, CSV conformance, state consistency, atomic flags, json-vs-csv, expand registry, dedup, rebuild correctness, doc accuracy | 10 |
| 7 | Python engineer | controller loop, HTTP server, lifecycle, concurrency, rebuild_json, gen_onion_html, vision, tui, bash helpers, docs | 10 |
| 8 | Frontend/UX | perf limits, a11y, IA, error states, interaction, XSS, terminal UX, review-page scale UX, consistency, docs | 10 |
| 9 | Documentation specialist | completeness, accuracy (25+ spot checks), consistency, navigation, onboarding, terminology, comments, examples, gaps, index | 10 |
| 10 | LLM/API integration | prompts, output robustness, retry/backoff, 429s, cost, determinism, prompt-injection, multi-provider, vision, docs | 10 |

## Consolidated findings (de-duplicated across personas)

### CRITICAL
| ID | Finding | Status |
|---|---|---|
| C1 | API key written to predictable `/tmp/depot_llm_<pid>_<seq>.hdr` with chmod-after-write → symlink planting / key theft | **FIXED** — `mkstemp` (0600 at creation, unique names) in `curl_post` |
| C2 | Unauthenticated `/api/threads` up to 5000 → remote money-DoS when bound to 0.0.0.0 | **MITIGATED (docs + hardening)** — POST clamp hardened; exposure guidance kept; token-auth noted as open item |
| C3 | `body_failed` accepted proxy HTML error pages (curl -s exits 0 on 502) → nodes "succeed" on failures | **FIXED** — now requires a `"choices"` field in the body |
| C4 | No `finish_reason` check — truncated-at-max_tokens JSON committed as final data | **DOCUMENTED (open item)** — requires response-format change; tracked below |

### HIGH
| ID | Finding | Status |
|---|---|---|
| H1 | `drained()` read `queue` without the mutex — data race on `std::deque` | **FIXED** — lock inside `drained()` |
| H2 | `std::async` fan-out had no exception containment → `std::terminate` on thread exhaustion | **FIXED** — try/catch around `work()`; settle-on-exception |
| H3 | Trimmer API failures silently swallowed (stale candidates settled) | **FIXED** — failures counted; 2/2 failing rounds → node failure + retry |
| H4 | Critique parse failure defaulted to score **1.0** (fail-open → settle garbage) | **FIXED** — default 0.0 (retry, then settle); tests renamed |
| H5 | Stored XSS: 300MB payload inlined into `<script>` without `</script>` escaping | **FIXED** — `</` → `<\/` + U+2028/29 escaped in `gen_onion_html.py` |
| H6 | Hysteresis was cosmetic — `write_control()` every loop bypassed it | **FIXED** — control file only written on committed changes |
| H7 | `reasoning_param`/`default_model` keyed on key prefix only, ignoring `DEPOT_API_URL` | **FIXED** — endpoint override now decides the reasoning knob first |
| H8 | L9 nodes never settled → re-queued/re-counted/re-logged every round | **FIXED** — settled (kept=0) on first touch |
| H9 | systemd example supervised a dead wrapper script (`onion-start` double-forks) | **FIXED in docs** — foreground `ExecStart` of `controller.py` |
| H10 | Controller crash-loop unbounded (restart forever on bad model id) | **FIXED** — cap 5 restarts, then give up + log |
| H11 | `funnel_state.tsv` gitignored — months of settlement history unprotected | **DOCUMENTED** — backup guidance added to OPERATIONS; tracked as open item |

### MEDIUM (fixed)
| ID | Finding | Status |
|---|---|---|
| M1 | `/api/threads` crashed the handler on non-numeric `max` | **FIXED** — validated, 400 on bad input |
| M2 | `onion-stop` left `control/stopped` → next start born "stopped" | **FIXED** — flags cleared in onion-stop |
| M3 | pgrep pattern `controller\.py --` missed arg-less controllers (tui spawn) | **FIXED** — anchored on `controller\.py` |
| M4 | `onion-start --bind 0.0.0.0` misparsed as seed | **FIXED** — `--bind` case added |
| M5 | `vision.py` hardcoded OpenRouter+gemini; legacy DeepSeek key silently 401'd; zero error handling | **FIXED** — key-driven URL/model + `HTTPError`/empty-content handling |
| M6 | `logf` fd leaked per restart; duplicate `os.makedirs` | **FIXED** |
| M7 | Docs: test counts (83/82), poll 3s, clamp 1..64, phantom `node_tools.tsv`, broken workflow link, reasoning-knob contradiction, `next_tk` scope, `--force` state-row wording | **FIXED** across 8 doc files |

### MEDIUM/HIGH — accepted as open items (documented, not fixed this round)
| ID | Finding | Why deferred |
|---|---|---|
| O1 | `finish_reason` not checked (C4) | response-format change; needs wire-format decision |
| O2 | No HTTP-code capture / `Retry-After` / backoff-jitter on 429s | transport rewrite (in-process HTTP client) — larger refactor |
| O3 | `run_llm` has no `AskFn` seam → half the LLM surface untested | interface change + new tests; planned |
| O4 | Single global mutex serializes all logging + file writes (scaling ceiling at high threads) | spooler refactor; measured as secondary to LLM latency |
| O5 | 40k-thread `std::async` worst case; no global clamp | semaphore design; current adaptive policy bounds practice |
| O6 | In-place (non-atomic) CSV rewrites in `run_ingest`/`run_llm` | tmp+rename refactor |
| O7 | `next_tk` (ingest) scans L5+L6 only | funnel scans all; documented, cross-level collision risk accepted |
| O8 | Duplicate paths (131) with conflicting titles in live data | requires a deliberate data-migration pass on the corpus |
| O9 | Stale `Onion.json` vs CSVs after manual (non-controller) runs | run `rebuild_json.py` after manual runs — documented |
| O10 | Controller API auth token / CSRF protection | product decision (loopback default + docs warning today) |
| O11 | State/ledger backups offsite | needs infra choice (git-track vs cron rsync) |
| O12 | Accessibility (keyboard, ARIA, reduced-motion) across UIs | scheduled UI polish |
| O13 | `onion_gl` ESC quits on arrow keys (terminal mode) | input-sequence parsing |
| O14 | 60s rebuild chain has no `finished` gate (idle churn) | small; batched with O2 |

## Verification

- `cmake --build build -j$(nproc)` — clean
- `ctest --test-dir build` — **92/92 passed** (84 core + 8 GPU), including updated fail-closed critique tests
- `bash -n` onion-start/onion-stop · `py_compile` controller/vision/gen_onion_html — clean
- Live funnel smoke test after fixes: `kept=16 atomic=16 errors=0` on the DeepSeek key

## Re-review note

A second fleet round (passes 11+) should re-check C1/H5/H7 fixes in place and re-grade O1–O4 before the next concurrency scaling push (500–5000 threads).
