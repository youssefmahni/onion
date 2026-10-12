# OPT track — WORKLOG (append-only, latest first)

Entries follow the format from OPTIMIZATION_GOAL.md §8:

```
## <UTC time> · <workstream> · <task>
Result: done | in progress | blocked
Evidence: <commit hash, command + last lines of output, CI link>
Attempts: <only for problems: n. hypothesis → change → evidence>
Next step: <one precise action>
```

## 2026-10-11 · W3 · local pass while CI was billing-blocked: traceability audit, fuzzers, C4, C5, fold idempotency

Result: done (CI was blocked; now public-repo CI is green and the branch awaits merge)
Evidence: TRACEABILITY_AUDIT.md; tests/fuzz/ (5 targets, clean); C4 kill points 1-4 in
tests/unit/state_test.cpp; the doubled-and-shuffled replay test (StateWriterFoldIdempotency.*);
attempts/critiques keyed by (origin, hlc); settle fold score-first; decision 0015; C5 smoke
(2 billed calls, HTTP/2). Full suite green locally.
Next step: merge in the owner's order (#21 done, m6-corrections done) and land this branch.

## 2026-10-11 · W1 · setup: worktree, corpus copy, probe tool, audit (3 PRs)

Result: done
Evidence:
- Worktree `~/Desktop/onion-opt` created from `origin/main` (`c074d9e`), branch `opt/setup`.
- Corpus copied once with rsync to `~/onion-opt-data`; per-level counts verified
  against OPTIMIZATION_GOAL.md §4.1: L0=9 L1=17 L2=95 L3=818 L4=15,779
  L5=91,047 L6=208,499, total 316,264 — exact match. funnel_state rows = 11
  (brief: "about 12"), atomic rows = 29 (brief: "about 30"); recorded in AUDIT.md.
- PR #13 `opt/setup` (goal file + COORDINATION.md): merged, CI green
  (run 38141130468, 8 jobs × 2 runs).
- PR #14 `opt/w1-audit` (AUDIT + memory files): merged, CI green
  (run 38141635568, 16 checks pass).
- PR #15 `opt/llm-probe` (probe tool + evidence CSVs): merged, CI green
  (run 38142453232, 16 checks pass).
Attempts: 1. Windows CI failed on the probe target: MSVC `max`/`min` macro
clash (error C2589 at deepseek_concurrency.cpp:147,148,249,279,463) → the
`llm` module already defines NOMINMAX; added the same definitions to the
probe target in CMakeLists.txt → all 16 checks pass.
Next step: W2 — write decision record "Limiter v2" and the limiter v2 tests
first (TDD), then implement in src/llm/.

