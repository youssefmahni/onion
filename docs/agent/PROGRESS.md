# Progress

## Current

- Milestone: M1 · branch m1-foundations · started 2026-10-09
- Tests: legacy 98/98 · new 81/81 on Linux · CI: pending task-6 checkpoint

## Log (newest first)

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
