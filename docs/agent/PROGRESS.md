# Progress

## Current

- Milestone: M2 · branch m2-deepseek-client · started 2026-10-10
- Tests: legacy 98/98 · new 95/95 on Linux (87 passed + 8 skipped on macOS/Windows) · CI: M2 green

## Log (newest first)
- 2026-10-10: Diagnosed the 1,000-request client crash with ASan/UBSan as a queued-deque iterator use-after-free; fixed by draining ready tasks under the mutex before curl work · normal and sanitizer benchmarks now complete 1,000/1,000 against the fake server · added fake-server concurrency and transport-failure regression coverage · no real calls

- 2026-10-10: M2 checkpoint: added standard-library fake DeepSeek server, scenarios and adaptive limiter tests · CI 56aa921 green on Linux, macOS arm64, macOS Intel compatibility build and Windows · no real calls; 1,000-request benchmark not started
- 2026-10-10: M2 started from merged M1; no real DeepSeek completions approved or used · pricing source recorded in decision 0004
- 2026-10-10: M1 tasks 7–9: added RFC and project golden vectors, confirmed CI on Linux/macOS/Windows, and completed decision records · local new 88/88 · legacy 98/98 plus controller 9/9 · gate report prepared
- 2026-10-10: M1 task 6 checkpoint: corrected durable replacement semantics and platform sampler/key-store/data-directory coverage · local new 83/83 · CI green on all jobs
- 2026-10-09: M1 task 6: implemented the platform data directory, durable append, atomic replace, sampler and staged key store · local new 81/81 · GPU tests cannot run in hosted CI on real fp64 hardware; full GPU validation needs the Mac Pro runner
- 2026-10-10: M1 task 6 follow-up: battery sampling on macOS/Linux and GPU-load sampling are deferred to M5.
- 2026-10-09: Corrected TLS policy: macOS SDK curl, Linux distribution OpenSSL curl, Windows pinned Schannel curl; minimum M2 curl version 7.66.0 · no real API calls
- 2026-10-09: M1 task 3: added Linux, macOS arm64, macOS x86_64/12.0 compatibility-build, Windows, and legacy CI workflow jobs · local new smoke 1/1 · CI cannot be triggered because this checkout has no remote and no GitHub CLI
- 2026-10-09: Started M1 task 1; confirmed the POCL cache failure was sandbox-only and the legacy suite passes 98/98 with `POCL_CACHE_DIR` in a temporary writable directory · no real API calls · M0 remains open and untagged by owner decision

## Real API usage

| Date | Milestone | Purpose | Calls | Cost (USD) |
|---|---|---|---:|---:|

## Background processes started

- none
