# Onion Engine — Rebuild Plan

Oct 9, 2026 · @y.mahni

## Summary

Rebuild the engine in ten milestones, M0 to M9, keeping the pure algorithmic core and its tests and rewriting everything around it to match the design doc (`docs/DESIGN.md`). Each milestone ships on its own and ends at a gate that must pass before the next starts.

**Strategy**

- **Keep what is proven.** `depot_core` (canonical titles, similarity, parsers, style gates) and `depot_gpu` remain unchanged in `legacy/`, which retains all 98 tests; M1 copies 67 pure-core tests and 8 GPU tests (75 total) into the new tree as its regression oracle.
- **Rewrite what blocks the design.** Funnel orchestration, LLM transport, storage, supervisor and CLI are rewritten as the modules in the design doc: one process per machine, SQLite as the store, signed logs as the truth, DeepSeek as the only provider.
- **Drop what no longer fits.** Image understanding (`vision.py`), the bash launch scripts, the duplicated thread-policy code and the stale report scripts.
- **Single machine first, fleet second.** The quality gate and the local engine (M3) are finished before any sync (M6), so every machine that ever joins produces checked output.
- **Today's system keeps running.** M0 fixes today's code so it works during the rebuild; the switch to the new system happens at M4, with a rollback path until M6.

**Done means:** a macOS, Linux and Windows fleet on the home LAN runs a campaign for 24 hours, converges to one Merkle root, stays within budget, survives power cuts and partitions, and the old code is deleted.

## Keep, rewrite, drop

Two parts of today's code are kept as they are, most of the rest is rewritten, and four groups of files are dropped.

| Today | Fate | Becomes | Milestone |
| --- | --- | --- | --- |
| `depot_core` and its tests | Keep | Pure core, moved to `src/core` | M1 |
| `depot_gpu` and its tests | Keep | Optional OpenCL similarity, `src/gpu` | M1 |
| `run_funnel`, `work()` in depot_io.cpp | Rewrite | `depot_gate`, `depot_sched`, `depot_state` | M3 |
| `curl_post` (popen, mkstemp) | Rewrite | `depot_llm` on libcurl multi | M2 |
| Level CSVs as storage, L9 guard | Rewrite | SQLite store; CSVs become an export | M3, M8 |
| `funnel_state.tsv` and audit TSVs | Rewrite | Genesis import into signed logs | M4 |
| `run_ingest`, `run_llm` | Rewrite | Emit records; `run_llm` gets a test seam | M3 |
| main.cpp CLI | Rewrite | `node`, `campaign`, `review` subcommands | M3, M5 |
| controller.py | Rewrite | Sampler, policy and API inside the node | M3, M5 |
| dashboard.html, tui.py | Keep, adapt | Read the new `/api/state` | M3 |
| rebuild_json.py, gen_onion_html.py | Rewrite | SQLite queries; review page loads lazily | M3 |
| `vision.py` | Drop | Image understanding is out of scope | M1 |
| `onion-start`, `onion-stop` | Drop | `node start`, `node stop` on every OS | M5 |
| `depot_core_wasm.js`, dashboard JS policy, tui.py policy | Drop | One request limit in `depot_llm` | M2 |
| gen_funnel_report.py, demo_preview.cpp | Drop | Stale paths and model ids | M1 |
| onion_gl | Keep, frozen | Optional, Linux with X11 only | — |

## Target repository layout

One repository, one CMake build, one binary; old code lives in `legacy/` as the rollback path until M6 passes and is deleted in M9.

```
onion/
  CMakeLists.txt
  CMakePresets.json          macos, linux, windows presets
  third_party/               libcurl, cpp-httplib, monocypher, sqlite, zstd, json.hpp, OpenCL headers, googletest
  src/
    core/                    canonical titles, similarity, parsers, style gates (kept)
    gpu/                     OpenCL similarity (kept, optional)
    platform/                durable write, rename, sampling, key store, data dir, services
    profile/                 hardware scan, derived settings, versions
    llm/                     async DeepSeek client, limit, metering, reservations
    gate/                    pre-check, funnel agents, checks, best-of-3, leaf rules
    sched/                   campaigns, lanes, stealing, claims, shares, budget chunks
    state/                   state writer, ticket index
    log/                     records, batches, signing, segments, torn tail
    store/                   SQLite schema, fold, checkpoints, Merkle, compaction, exports
    swim/                    membership, beacon, gossip
    sync/                    vectors, pulls, notices, HTTP endpoints
    api/                     local HTTP API (/api/state, review, campaigns)
    cli/                     main: node, campaign, review subcommands
  tests/
    unit/                    per module, plus 75 copied core/GPU regression tests
    golden/                  cross-OS byte-identical canonical bytes, hashes, signatures
    fleet/                   several nodes in one process, in-memory network
  tools/
    fake_deepseek/           scripted latency, 429, 503, truncation, bad JSON
    chaos/                   kill points, partitions, clock jumps, full disk
    import/                  genesis import and its verifier
  web/                       dashboard.html
  tui/                       tui.py (optional)
  docs/                      design doc export, decision records
  legacy/                    today's code until M9
```

## Module layers

The new code is built bottom-up in five layers; each module may call only modules in the layers below it, which keeps the pure core testable and the OS calls in one place.

*(The original doc shows this as a diagram; same content as a table.)*

| Layer | Modules |
| --- | --- |
| Interface | cli, api |
| Engine | gate, sched, state |
| Fleet | sync, swim, profile |
| Data | log, store, llm |
| Base | core, gpu, platform |

Only the base layer's core and GPU modules carry over from today; the platform module is new, and every layer above is rewritten to the design doc.

## Milestones

Ten milestones in strict order; each needs the one before it, and sizes are relative (S, M, L), to be turned into dates once M0 shows real velocity.

| Milestone | Builds | Exit gate | Size |
| --- | --- | --- | --- |
| **M0 · Stabilize today's code** | P1 to P7 in the old code; full off-machine backup of CSVs and TSVs; duplicate-path report | One real DeepSeek node succeeds on a Mac; the P7 regression test passes | S |
| **M1 · Foundations** | New repo layout; CMake presets; CI on macOS, Linux, Windows 10 and 11; vendored libraries; `depot_platform`; core and GPU copied with their tests; dropped files removed | The 75 copied tests pass on every target (GPU tests skipped where no OpenCL runtime exists); a golden test produces identical bytes on every OS | M |
| **M2 · DeepSeek client** | `depot_llm`: async requests, metering, reservations, adaptive limit, backoff, circuit breaker; `tools/fake_deepseek` | Against the fake server: 1,000 requests in flight on about 20 threads, the limit halves on 429, no budget overspend | M |
| **M3 · One-machine engine** | SQLite store, `depot_state`, `depot_gate` (pre-check, funnel, checks, best-of-3, leaf rules, review), `depot_sched` with campaigns and one lane, CLI, local API, dashboard on the new `/api/state` | A test campaign of a few hundred nodes on one Mac: no failed attempt is committed, review works from the dashboard, the cost projection is shown before start | L |
| **M4 · Signed log and genesis import** | Keys, 2^40 slot ids, batches, segments, torn-tail repair, checkpoints, admin key; the genesis import of the existing corpus; cutover from the old system | Import counts, parents and Merkle root match the cleaned CSVs; a cut at every byte offset keeps exactly the complete batches | L |
| **M5 · Profiles and service** | Hardware scan, profile versions, overrides; launchd, systemd and Windows Service; concurrency tuning on a Mac Pro | Profiles correct on all three OSes; the service restarts after a forced kill; a live Mac Pro run holds its limit without 429 storms | M |
| **M6 · Sync between known machines** | Vectors, pulls, co-signed checkpoints, new-task notices; home lanes over a fixed member list; campaigns and budget chunks across machines | Three Mac Pros run a campaign for 24 h, converge to one Merkle root and stay within one chunk per machine of the loop budget | L |
| **M7 · Dynamic fleet** | SWIM, beacon, invitations with seeds, majority rule, eviction, auto-retire, admin-key eviction; stealing with claims; re-expansion locks | A Linux machine joins mid-run; a Mac loses power; an idle machine steals without duplicates; a lone eviction is rejected | L |
| **M8 · Depth and scale** | Branch shares, depth checkpoints, light funnel, compaction, cost and growth projections, observability of lanes, claims, spend and disk | A campaign that goes past L9 ends within its pools; measured log growth matches the projection | M |
| **M9 · Mixed fleet and hardening** | Windows 10 and 11 machines join; security review; chaos suite; documentation; `legacy/` deleted | A 24 h soak on macOS, Linux and Windows converges; the chaos suite passes | M |

## Migration and cutover

The old system keeps producing until M4; the switch is one planned event with a verified import, a parallel run and a rollback path that stays open until M6 passes.

**Before the cutover (M0 to M3)**

- Today's fixed code (M0) keeps running campaigns on the Mac Pros.
- From M1, the genesis import runs as a dry run on a copy of the data every week, so data problems beyond the 131 known duplicate paths surface early.

**Cutover (M4)**

1. **Parallel run.** Run the same small campaign on the old and the new engine from the same starting data; compare child counts, atomic share and scores.
2. **Freeze.** Stop the old system and take a final off-machine backup.
3. **Clean.** Resolve the duplicate paths; you approve the dropped variants.
4. **Import.** One Mac Pro writes the whole corpus into its log as legacy records, in slot 0.
5. **Verify.** Counts per layer, every parent present, and the Merkle root against a fresh count of the CSVs.
6. **Checkpoint.** A second Mac Pro rebuilds the root independently and co-signs.
7. **Switch.** The new engine runs from here on; the CSVs become an export.

**Rollback.** Until M6 passes, the old binary and the final backup are kept. Work done in the new system after the cutover can be exported back to CSVs if a rollback is ever needed.

## Testing strategy

Five test layers, each added when its milestone needs it; nothing merges unless CI passes on every operating system.

| Layer | What it is | From | Proves |
| --- | --- | --- | --- |
| Unit | gtest per module, plus 75 copied core/GPU regression tests | M1 | Every rule in the design doc, one test per rule |
| Golden | The same inputs canonicalized, hashed and signed on each OS | M1 | Byte-identical results, so every OS folds the same state |
| Fake DeepSeek | `tools/fake_deepseek`: scripted latency, 429, 503, cut-off replies, bad JSON | M2 | Limit, reservations, retries and the gate behave under every failure |
| Fleet harness | Several nodes in one process over an in-memory network | M6 | Convergence, lanes, stealing, claims and merges under reordering and duplicates |
| Chaos | `tools/chaos`: kills at every commit step, partitions, clock jumps, full disk | M7 | No committed work lost, no duplicate paths, identical Merkle roots after healing |

**Live checks**

- **Real transport smoke test** with a real key on every release. The old suite only used mocks, which is how the `mkstemp` bug slipped through.
- **Soaks:** 24 hours on the Mac Pros at M6, then on the mixed fleet at M9.
- **Cost check:** every campaign compares its projected cost with the actual, and a gap over the tolerance you set raises an alert.

## Risks and mitigations

The biggest unknowns are external, DeepSeek's real limits and cost, so the plan measures them early, in M2 and M3, before the fleet work depends on them.

| Risk | Effect | Mitigation |
| --- | --- | --- |
| DeepSeek's real limits at high concurrency are unknown | Throughput far below the design's 1,000 requests | Adaptive limit; a small real run in M2 measures latency and errors before M3 |
| Cost per node is higher than expected | Budgets drain fast | Reservations, cost projection before every campaign, small first campaigns |
| libcurl's TLS setup differs per OS | A build or HTTPS failure on one OS | Settle the TLS backend for each OS in M1, in CI |
| macOS 12 on the Mac Pros | Hosted CI may no longer offer macOS 12; older toolchains | Use a Mac Pro as a self-hosted CI runner from M1 |
| The genesis import finds more bad data than the 131 duplicates | M4 slips | Weekly dry-run imports from M1 on a copy of the data |
| Windows specifics (services, DPAPI, paths, firewall) | Late surprises at M9 | Windows builds and tests in CI from M1, long before a Windows machine joins |
| Scope keeps moving during the rebuild | The rebuild never finishes | Freeze the design doc's sections for the current milestone; new ideas become decision records for later milestones |
| One person reviews every flagged node | Subtrees sit idle | Batch review and optional auto-accept, built in M3 |
| Prompt content goes to an external provider | Confidential material leaves the LAN | Keep sensitive scopes out of campaigns; check DeepSeek's retention terms before M3 |

## Working practices

A milestone is done only when its gate passes in CI and live, the design doc matches what was built, and a tagged release exists.

- **Design doc first.** A change to behaviour updates the design doc before the code; anything not yet decided becomes a short decision record in `docs/`.
- **Small changes.** One module or one rule per change, each with its tests; nothing merges unless CI passes on macOS, Linux, Windows 10 and Windows 11.
- **Review checklist,** even when working alone: tests added, design doc updated, no OS call outside `depot_platform`, no secret in logs or config files.
- **Tagged releases.** Each milestone ends with a tag (`m0`, `m1`, …) and short release notes, so any machine can be rolled back to a known state.
- **Measure, then tune.** The starting values in the design doc (gate, shares, leases, chunks, profile limits) are changed only from measured runs, and each change is recorded.

**Definition of done for each milestone**

- [ ] Exit gate passes in CI and in a live run
- [ ] Design doc sections for this milestone match the code
- [ ] New tests cover every rule added
- [ ] Release tagged with notes

## Open decisions

- [x] New repository, or the same repository with today's code moved into `legacy/`? *(Decided: new repository with today's code in `legacy/`.)*
- [ ] Use a Mac Pro as a self-hosted CI runner for macOS 12?
- [ ] Keep tui.py and onion_gl, or let the browser dashboard be the only interface?
- [ ] Which small campaign serves as the parallel-run comparison at cutover?
- [ ] The design doc's open questions (gate defaults, branch share, checkpoint interval, lease and chunk sizes) are settled with measurements in M3 and M8.
