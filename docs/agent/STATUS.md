# Onion Engine Status Report

Generated: 2026-10-10
Working branch: `m2-deepseek-client`
Current HEAD: `9006166` — `m2: merge legacy pool test stabilization`

## Executive summary

M1 Foundations is complete, merged to `main`, and tagged `m1`. M2 DeepSeek
client work is implemented substantially but is **not ready for its approval
gate**: the latest M2 CI run failed only in the Windows new-tree job. The
failure is the closed-port transport regression: Windows Schannel/libcurl
times out each loopback attempt instead of returning a refused-connection
transport result. No real DeepSeek completion calls have been made.

The separate legacy macOS timing failure was repaired with a test-only change,
merged to `main`, and passes on Linux and macOS CI.

## Milestone status

| Milestone | Status | Evidence |
|---|---|---|
| M0 Stabilize legacy | Open | Owner real-node DeepSeek validation remains pending; `m0` is not tagged. |
| M1 Foundations | Complete | Merged to `main`, tagged `m1`; M1 gate report: `docs/agent/gates/M1.md`. |
| M2 DeepSeek client | In progress | Fake service, limiter, client, error classification, accounting, benchmarks and sanitizer jobs exist. Windows CI regression still fails. |
| M3 One-machine engine | Not started | SQLite store, state writer, log, gate and scheduler remain future work. |
| M4 Signed log/import | Not started | Device identity, Ed25519 production signing and import remain future work. |
| M5 Profiles/service | Not started | Profile derivation, OS service installation and production key stores remain future work. |
| M6 Known-machine sync | Not started | HTTP synchronization, static membership and fleet harness remain future work. |
| M7 Dynamic fleet | Not started | SWIM, invitations, eviction and stealing remain future work. |
| M8 Scale/depth | Not started | Branch shares, compaction and scale observability remain future work. |
| M9 Mixed-fleet hardening | Not started | Windows fleet validation, security review, soak and legacy removal remain future work. |

## Current M2 implementation

### Implemented

- Standard-library Python fake DeepSeek service with `/models` and
  `/chat/completions`, scenario-driven responses, request logging without
  Authorization values, deep accept queue and threaded handlers.
- Scripted healthy, rate-limited, truncating and slow-under-load scenarios.
- C++ libcurl multi client driven only by `curl_multi_poll`.
- Stable heap ownership of every request body, header list and response buffer
  until its curl easy handle is removed.
- HTTP/2 TLS preference with HTTP/1.1 fallback; runtime capability reporting.
- CPU parsing pool sized to hardware concurrency; local benchmark showed a
  stable 14 process threads at both 100 and 1,000 active requests.
- Integer-decimal budget accounting and concurrent reservation protection.
- Timeout accounting: a timeout retains its full reservation as uncertain
  spend and is reported by the benchmark.
- Limiter rules, hourly probe-baseline model and trace tests.
- Circuit breaker classification for provider, transport, timeout,
  authentication and balance failures.
- Unix file-descriptor-limit reservation and request-ceiling cap.
- Linux AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer CI
  jobs.

### Benchmark evidence on Kali Linux using the fake service

| Run | Result |
|---|---|
| Capacity, limit 1,000 | 1,000 started, 646.96 req/s, p50 0.923 s, p95 1.440 s, p99 1.477 s, peak active 1,000, 14 process threads. |
| Connection reuse, limit 100 | 1,000 started, 100 connections opened, peak active 100. |
| Half-budget reservation | 500 started, 500 refused; final spend USD 0.50, below the USD 5.00 budget. |
| ASan/UBSan capacity | 1,000 successful requests, 120.12 req/s; no sanitizer finding. |
| Timeout outage | 1,000 calls with one-second deadline; Linux regression verifies Timeout classification, uncertain billing and breaker opening. |

The fake server is HTTP/1.1 only. The HTTP/2 path will first be exercised in
the approved real-call validation step. Linux libcurl 8.22.0 reports HTTP/2
support; the pinned Windows curl 8.11.1 reports no HTTP/2 support and uses
HTTP/1.1 fallback.

## Current blocker

Latest M2 run: https://github.com/youssefmahni/onion/actions/runs/38059588424

Green jobs in that run:

- Linux new tree
- Linux ASan + UBSan
- Linux TSan LLM client
- macOS x86_64 / 12.0 compatibility build
- Legacy Linux
- Legacy macOS

Failing job: Windows new tree.

The exact failure is `llm.closed_port_1000`. On the hosted Windows runner,
libcurl 8.11.1/Schannel reports all 1,000 attempts as `Timeout` after the
two-second deadline, with zero opened connections, rather than `Transport`.
The regression currently requires 1,000 transport results. This is the only
known reason M2 cannot yet pass its gate.

## CI and test posture

- Linux normal suite: 113 tests locally, all passed (including 8 GPU tests).
- macOS and Windows hosted new-tree suites: GPU tests are intentionally
  skipped when OpenCL is absent.
- Current M2 CI is not green due to the Windows closed-port classification
  above.
- Legacy macOS `PoolResizesFromControlFile` previously raced; the test-only
  fix sleeps 100 ms per fake reply. Its 20-repeat local check and green
  legacy Linux/macOS CI evidence are recorded in commit `7379cb3` / merge
  `62ae2f9`.

## Design decisions recorded

| Record | Subject |
|---|---|
| 0000 | Owner clarifications before M1 |
| 0001 | Repository layout |
| 0002 | TLS backend split and minimum libcurl |
| 0003 | Exact signed-64-bit record-number serialization |
| 0004 | DeepSeek model and peak/off-peak pricing |
| 0005 | Limiter probe-baseline policy |
| 0006 | LLM error outcomes and breaker behavior |

## Repository map

```text
onion/
├── CMakeLists.txt                 Root build and target dependency rules
├── CMakePresets.json              Linux, macOS and Windows build presets
├── onion.task                     Plain-language completed-task ledger
├── status_repport.md              This report
├── docs/
│   ├── DESIGN.md                  Authoritative architecture specification
│   ├── REBUILD_PLAN.md            Milestones and exit gates
│   ├── agent/                     Implementation brief, progress and gates
│   └── decisions/                 Architecture decision records 0000–0006
├── third_party/
│   ├── VERSIONS.md                Pinned dependency versions and licenses
│   └── OpenCL-Headers/            Vendored OpenCL headers
├── src/
│   ├── core/                      Copied pure parsing/canonical/similarity core
│   ├── gpu/                       Optional OpenCL operations
│   ├── platform/                  OS files, sampling, data paths and key store
│   ├── llm/                       Limiter, budget, breaker and curl-multi client
│   ├── log/                       Placeholder for signed operation log
│   ├── store/                     Placeholder for SQLite state store
│   ├── profile/                   Placeholder for device profiles
│   ├── sync/                      Placeholder for known-machine synchronization
│   ├── swim/                      Placeholder for dynamic membership
│   ├── gate/                      Placeholder for quality funnel
│   ├── sched/                     Placeholder for scheduling and lanes
│   ├── state/                     Placeholder for state writer
│   ├── api/                       Placeholder for local HTTP API
│   └── cli/                       `data_depot llm bench` and limiter trace CLI
├── tests/
│   ├── unit/                      Core, GPU, platform, LLM and fake-server tests
│   ├── golden/                    Cross-platform bytes/signature vectors
│   └── fleet/                     Reserved for M6 fleet harness
├── tools/
│   ├── fake_deepseek/             Scenario-driven fake DeepSeek server
│   ├── chaos/                     Reserved for M7 process/clock/disk chaos tools
│   └── import/                    Reserved for M4 corpus import tool
├── web/                           Reserved dashboard assets
├── tui/                           Optional terminal UI location
└── legacy/                        Existing engine retained through M9
    ├── depot_core.*               Legacy pure core
    ├── depot_gpu.*                Legacy GPU implementation
    ├── depot_io.*                 Legacy funnel and provider loop
    ├── depot_core_test.cpp        Legacy core/funnel tests
    ├── depot_gpu_test.cpp         Legacy GPU tests
    ├── controller.py              Legacy controller
    └── docs/                      Legacy technical documentation
```

## Immediate next work

1. Diagnose and make the closed-port regression portable on Windows while
   preserving the specified `Transport` semantics for refused connections.
2. Re-run all CI jobs until green.
3. Generate and commit the M2 gate report, then stop for owner review.
