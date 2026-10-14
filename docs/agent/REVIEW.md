# Independent review — takeover from Agent M (M1–M8) and OPT

Date: 2026-10-14 · Reviewer: takeover agent · Reviewed tree: `m8-depth-scale`
@ `d631886` (4 commits ahead of `origin/main` @ `cb0f3ab`).

Method: every claim below was re-derived from code, tests, CI logs or command
output. Code review of sync/budget/security/funnel was split across three
read-only reviewers; each finding marked **[spot-checked]** was re-read by the
takeover agent at the cited lines. Findings not so marked are the reviewer's
reading of the code and carry its evidence (file:line) for re-checking.
Estimates are labelled as estimates.

Legend: ✅ verified · ⚠️ partial · ❌ false or missing.

---

## 0. Findings, sorted by severity

IDs are stable; WORKLOG.md tracks fixes against them. "Plan" says where the
fix is scheduled. Critical and High get a failing-first test (B1); Medium and
Low go to the WORKLOG backlog.

### Critical

| ID | Finding | Proposed fix | Plan |
|---|---|---|---|
| F-GATE-1 | **The shipped quality gate cannot commit.** Defaults are gate 0.7 / floor 0.5 (`src/sched/sched.hpp:28-30`, `sched.cpp:68-73`, `gate.hpp:100-103`); the critic's measured maximum is 0.62 and its AUC vs blind judges is 0.54 (DR 0020-critic validation). Every non-leaf node burns 3 attempts and lands in review. The stop rule's score clause (average ≥ 0.8 over 50 nodes, `sched.cpp:224-236`) is unreachable, so campaigns never stop on quality. 0.35/0.15 is **not** a default anywhere (only a campaign-file override used in the W5.4 experiment; PR #36 only made records accept decimal gates). | New decision 0024: commit on deterministic checks, critic behind `critic: on|off` (default off), stop rule on verified atomic share only. | B3 |
| F-GATE-2 | **A provider outage permanently destroys the tree below it.** Any non-Ok LLM result marks the attempt failed (`gate.cpp:35-37`); after 3 failed attempts `decide()` sees no valid children and returns `Leaf` / "cannot split" (`gate.cpp:382-389`), recorded as `SETTLE end="cannot split"` (`sched.cpp:1037-1049`) and never reviewed. A 401, 402 (balance), timeout or outage therefore turns every node into a permanent leaf. `CircuitBreaker` is not wired into the campaign engine (only probe/benchmark commands, `src/cli/main.cpp:151,214,348`). **[spot-checked: decide() at gate.cpp:366-389]** | Distinguish "the model said no split" from "the call failed": a failed call never commits a leaf; pause the node, trip a breaker on auth/balance errors, stop the campaign loudly. | B1 |

### High

| ID | Finding | Proposed fix | Plan |
|---|---|---|---|
| F-CI-1 | **CI on `m8-depth-scale` is red, not "7 of 8 green".** Run 38556586929 (head `d631886`): only *New tree (macOS arm64)* fails — `sync.multi_process_convergence` "FAILED: quiescence" after 731 s. Run 38553880466 (previous head): *New tree (Windows)* failed. M8 gate report says "CI confirmation pending OA-14 (billing)" but CI is running (runs completed in 17–25 min), so OA-14 looks stale. | Fix the test (see F-TEST-1), rebase on main, re-run. | B2/B5 |
| F-TEST-1 | `quiescence` is `DeadlineExceeded` in `multi_process_test.py` step 2: it needs 3 identical 15 s samples of every node's vector (≥ 60 s silence) but shares the single 720 s global deadline already consumed by the in-flight chunk drain. Most likely test timing, not a product bug; a liveness bug cannot be excluded without the failure's sync debug lines. | Give step 2 its own window; dump per-node state + `ONION_SYNC_DEBUG` verdict lines on failure. | B2 |
| F-SYNC-1 | **`NonceCache` is a bare `std::map` with no mutex** (`src/sync/sync.hpp:50-60`, `sync.cpp:140-153`), called from httplib's thread pool (`sync.cpp:368`). Data race / UB on concurrent requests. **[spot-checked]** | Add a mutex; TSan test with concurrent verified requests. | B1 |
| F-SYNC-2 | **Majority denominator can include evicted/left members.** The DB count excludes EVICTED/LEFT (`state.cpp:1491`) but `commands.cpp:1392-1402` takes `max(db_count, members.json size)` and `members.json` is never pruned; peers contacted (`member_urls`) also include EVICTED/LEFT rows. Numerator and denominator disagree, and the 0014 escape hatch (evict a stuck member of a 2-fleet) does not restore progress while the file still lists it. **[spot-checked]** | Filter `members.json` ids and peer URLs by current-member state; end-to-end test: evict then a 1-of-1 round opens. | B1 |
| F-SEC-1 | **An evicted or departed device can still authenticate** to the sync server: `member_public_key_on` has no state filter (`state.cpp:995-1007`), so it can read logs, push batches and evict. **[spot-checked]** | Authenticate only CURRENT members (the push-address path already does). | B1 |
| F-SEC-2 | **Nonces carry ~32 bits of entropy, not ≥128** (decision 0018 point 1): `std::mt19937_64 generator(entropy())` seeded from one 32-bit `random_device` draw (`sync.cpp:72-73`, same pattern at `175-176`); hex is not zero-padded; no test checks width or freshness. **[spot-checked]** | Draw 16 CSPRNG bytes (platform RNG), pad to 32 hex, add width/freshness test. | B1 |
| F-FUN-1 | **ATTEMPT rows under-count tokens and cost.** `attempt.tokens_*` and `cost_usd` are snapshotted at `gate.cpp:673-676`, before the synthesizer (`:690-698`) and the verification call (`:723-727`) are metered; nothing re-assigns them. Everything derived (measure_params tokens/call, QualityGuard, cost reports, COST_MODEL parameters) excludes the largest-output call. | Snapshot after the last call of the attempt; test with a fake whose synthesizer reply is large. | B1 |
| F-BUD-1 | **The USD budget never decrements.** `commands.cpp:897` hard-codes `Budget("1000")`; the ticket worst case is a constant `"0.02"` (`commands.cpp:1329`); every reservation settles with actual `"0"` (`sched.cpp:966,1168`); no `node budget set` exists. DESIGN (519-552) promises a per-device USD budget; only the loop budget bounds spend. | Settle with the attempt's real cost (needs F-FUN-1); wire `node budget set`. | B1 |
| F-M8-1 | **M8 exit criterion "measured log growth within ±25% of projection" is not demonstrated.** `log_growth_matches_projection` (`sched.cpp:1553`) is called only by unit tests with synthetic numbers (`sched_test.cpp:513-529`); no campaign compares projected to observed bytes. Gate report M8.md lists it as met. **[spot-checked: grep of src/ and tests/]** | An engine test that runs a fake campaign, reads `plan.expected_log_growth_bytes` and the observed on-disk log size, and asserts the band. | B1/B5 |
| F-DOC-3 | **COST_MODEL.md misleads money decisions** (details in §A7): tables A/B/C mix assumed and measured inputs, mislabel scope, quote growth 7.14 but compute with 7.0, and table A's token row contradicts its own cost row ($3,356 corresponds to 1.55 B in / 2.41 B out, not 1.02 B / 1.59 B). | Regenerate every table from the B4 measurements with explicit scope and input provenance. | B4 |

### Medium

| ID | Finding | Plan |
|---|---|---|
| F-SYNC-3 | Read-only SQLite per-thread connections are only partly applied: `GET /sync/v1/merkle` (`sync.cpp:816`), SyncClient reads (`sync.cpp:888-1067`), join/evict handlers and API action handlers still use the shared writer handle. | backlog → B2 if the saturation test shows starvation |
| F-SYNC-4 | A fork entry is recorded before the pushed batch's signature/hash is verified: any authenticated member can plant `forks` entries in `/api/state` (deque capped at 64). | backlog |
| F-SYNC-5 | `StateWriter::campaign_budget()` (`state.cpp:1422`) reads `campaign_budgets_` without `index_mutex_` while fold writes under it (pre-existing since M6). | B1 (cheap, TSan) |
| F-SYNC-6 | Push classification tests cover only head batch #1; no test for an identical older batch (200), an older fork, or the gap 409, although the code path exists. | B1 tests (no product change expected) |
| F-SYNC-7 | `sync.partition_budget_majority`: commit `462183a` (after the recorded 20/20 proof) loosened `now > pre_split` to `now >= pre_split`; the shipped version has no recorded 20/20; the overspend bound is checked once, before the fill loop; nothing asserts nodes 4/5 reached each other. | B2: restore a strict progress assertion; 20/20 on the final binary |
| F-BUD-2 | The deterministic 3-device/15 s-lag simulation promised by 0014 does not exist (grep, `git -S`). Chunk sizing `max(50, ceil(1.2·window))` (`sched.cpp:899-907`) is untested above 41 attempts (a constant 50 passes). The permutation test's HLC ranges do not overlap, so the origin tie-break is untested. | B1 tests |
| F-SEC-3 | Local API: `GET /api/state` and `GET /` are unauthenticated and there is no Host check (DNS rebinding can read spend, member ids, Merkle root, reviews). Token = `"t-" + blake2b(steady_clock ns)` (not CSPRNG), compared non-constant-time, accepted via `?token=`; no payload limit before auth. `/api/stop` rejects `{"leave":true}` (400), so `leave` is unreachable. | B6 hardening |
| F-SEC-4 | `ONION_CHAOS_HOOKS` is a PRIVATE define on `data_depot` (`CMakeLists.txt:387`); the `ONION_SIM_DISK_FULL` hook in `src/platform/platform.cpp:182` never compiles, even with hooks ON — dead code, and the release-ignores-hooks test passes it vacuously. **[spot-checked]** `m7.release_ignores_chaos_hooks` runs only in hookless jobs (macOS release, ASan); CI never runs the `linux-release` preset; the clock-jump knob has no assertion. | B6 (disk-full chaos needs the hook) |
| F-SEC-5 | Sync server defaults to `0.0.0.0` in `SyncServer::Options` (`sync.hpp:100`) though the CLI default is 127.0.0.1; SWIM/beacon UDP bind `INADDR_ANY`; no LAN-only enforcement; no Windows firewall code. `X-Onion-Sync-Url` and the destination server are not in the signed bytes. Log/segment handlers use the URL origin as a path without validation (member-authenticated traversal, low). | B6 |
| F-SEC-6 | Windows key dir is not ACL-restricted (`private_dir` no-op; DPAPI blob via plain ofstream) and untested; a pre-existing loose-mode key file is briefly written before chmod. | B6 |
| F-SEC-7 | DESIGN threat-table rows that are false or unverified: "ingest strips instruction-like phrases" (no such code); "two device signatures" on checkpoints (cosignature is always empty and never verified); stranger-on-sync, replay, stolen device, LLM key, prompt injection, checkpoint rows PARTIAL. | B6 SECURITY_REVIEW.md |
| F-FUN-2 | Verification runs on every non-failed attempt that has an atomic label, even rejected ones (one call each); `ATOMIC` has no store effect (`state.cpp:819-821`) so "verified atomic" children are still expanded; ATOMIC records are written `verified:true` unconditionally (`sched.cpp:1072-1075`). | B3 (verify only the committed attempt) |
| F-FUN-3 | Deterministic-check failures are never fed back to the retry (only the previous critique `issues`, only into the planner prompt); `review retry --note` is never read by the funnel; restart does not resume attempt numbers (DESIGN:375/489/743). | B3 (inject reasons) / backlog |
| F-FUN-4 | `critic_model` replaces the model for ALL funnel calls (`sched.cpp:468`), DESIGN says critique only. A 31–100-child reply fails the count check rather than being truncated to 30 (`gate.cpp:705`). | B3 |
| F-FUN-5 | `settled_samples` for the stop rule is in memory only and resets on restart/re-entry (`sched.cpp:491`). | B3 (stop rule rewrite) |
| F-FUN-6 | No audit sampling exists (`audit_rate`, non-blocking audit review items); `auto_accept` is parsed but unused. | B3 |
| F-DOC-1 | No `docs/decisions/INDEX.md`; two records numbered `0020`; 16 records lack a `Status:` line (0000–0010, 0013, 0014, 0018, 0019, branch-share 0020). | INDEX.md added in this PR; renumber in B5 |
| F-DOC-2 | Stale labels: 0013 says "(Proposed)" though shipped; 0021's title says "(Proposed)", status Accepted; 0017 body still says "Decision needed"; dangling "DR 0018" gate references (`gate.hpp:153`, `gate.cpp:266`, GATE_CALIBRATION.md, FUNNEL_OPT.md). | B0 / B3 |
| F-DOC-4 | DESIGN.md contradicts Accepted records: token bucket 10/s vs 50 (0011); Keychain-first vs file-first (0019); signing text omits the nonce/device_id binding (0018); budget text omits majority rounds and the closed gate (0014 am. 2); light funnel L8 vs L6; 3–100 children vs 8–20/max 30 (0017); funnel steps omit batched verification (0021); open questions §821-828 all stale. Full list in §2. | B3 (gate/funnel/stop rule) + B0 docs |
| F-M8-2 | `DepthCheckpointSpendsNothingAndReleasesOnAccept` asserts attempts, not dollars, as the "spends nothing" proxy. | backlog |
| F-TEST-2 | On macOS the m7 fleet and partition tests exit 77 (no `ONION_CHAOS_HOOKS` in the macOS release preset); the partition test never runs on macOS or under sanitizers. | B2/B6 |

### Low

| ID | Finding |
|---|---|
| F-ENV-1 | Three orphaned `data_depot node run` processes (pids 291679–291681, `ppid` = systemd --user, started Oct 13, test dir `.build-tmp/onion_mp_qpcjxfd5`) were found running on the reviewer's machine; they come from an earlier `sync.multi_process_convergence` run that did not clean up on abnormal exit. The reviewer was not permitted to kill them (left running; ~2 % CPU each, slightly skews load tests). The test harness should kill its children in a `finally`/process-group teardown. |
| F-SYNC-8 | `run_round` reuses one timestamp for the whole round, so a round over 60 s gets 401s; edge replay gap for clients whose clock runs ~60 s ahead; no end-to-end test of the "replayed nonce" 401 response. |
| F-SYNC-9 | The 1-member gate test uses the SWIM live list, not the same total as `majority_round`; a `note_sync_round()` before the first `run()` is lost (costs one round). |
| F-SEC-8 | `.gitignore` lacks `keys/`, `*.key`, `.env.*`, `*.p12`. `ONION_EVICT_ABSENT_S` and `ONION_SYNC_DEBUG` are unguarded in release. `/sync/v1/checkpoint` reads the `.db` fully into memory. |
| F-FUN-7 | `QualityGuard` default threshold 0.4 > measured critic mean 0.31: a healthy run reads as flagged; `flagged` is not wired to lane weight (DESIGN:443/487). |
| F-DOC-5 | STATUS.md reports M2 (generated 2026-10-10); CUTOVER.md and REBUILD_PLAN.md predate M7/M8. |

---

## A1. Repository state

| Claim | Evidence | Status |
|---|---|---|
| Agent M's state: `m8-depth-scale`, "13 commits ahead of main", CI 7 of 8 green, macOS arm64 pending | `git rev-list --count origin/main..m8-depth-scale` → **4** (the other commits merged via PRs #40/#41, `cb0f3ab`). CI: run 38556586929 on `d631886` = **failure** (macOS arm64); run 38553880466 = failure (Windows). | ❌ |
| main is green | `gh run list --branch main`: CI on `cb0f3ab` (run 38512298081) success; OPT nightly 38537023009 success (2026-10-14). | ✅ |
| Open PRs | `gh pr list --state open` → none. #40 and #41 merged 2026-10-13/14. | ✅ |
| Tags | `m0 m1 m2-rc … m7-rc` (no `m8-rc`). | ✅ |
| Worktrees | `git worktree list`: `~/Desktop/onion` (m8), `~/Desktop/onion-opt` (main @ `1d4640f`, 9 behind `origin/main`, clean), `/tmp/onion-legacy` (prunable). | ✅ |
| Is another agent still active? | OPT's last commit `1d4640f` 2026-10-14 00:45 UTC; OPT worktree clean. No other author activity found; no OPT process found. The leaked test nodes (F-ENV-1) are not an agent. OPT is considered finished (its FINAL_REPORT.md exists). The takeover agent does not touch `~/Desktop/onion-opt`. | ✅ |
| Stale branches | many merged `m1…m7-*`, `opt/*`, `fix-legacy-pool-test-*` branches remain on the remote. Cosmetic. | ⚠️ |

## A2. Docs consistency

See `docs/decisions/INDEX.md` (new in this PR). Duplicate number: **0020**
(`0020-branch-share-pool-ledger.md` on `m8-depth-scale` and
`0020-critic-rubric-calibration.md` on main). The task's examples (two
records numbered 0013 or 0018) do **not** occur on this tree. DESIGN.md vs
Accepted records: contradictions listed in F-DOC-4 and §2. STATUS.md is
stale (F-DOC-5) → B0.

## A3. Claimed fixes verified in code and tests

### Sync (Part 0)

| Claim | Evidence | Status |
|---|---|---|
| Separate scheduler thread, own 15 s timer, no overlap | `src/cli/commands.cpp:1449-1475` single thread, steady clock, "sync round overran; skipping a tick"; joined at `:1719`. No test for the skip branch. | ✅ |
| Per-handler-thread read-only SQLite connections | `StateWriter::read_db()` `state.cpp:1062` (`thread_local`, `SQLITE_OPEN_READONLY|NOMUTEX`) used by sync auth/join/push/vector and `/api/state` (`commands.cpp:214`); merkle handler, client reads, join/evict, API actions still on the writer handle | ⚠️ F-SYNC-3 |
| Push verifies on the handler thread, enqueues, 202 | `sync.cpp ~575-701`; writer re-checks dup/fork `state.cpp:1160+` | ✅ |
| 409 classification by exactly (origin, number) | `stored_batch_hash_on()` selects from `batches` by (origin, number): identical older batch → 200; different hash → 409 + `note_fork` → `/api/state` `forks` (`commands.cpp:276`); number > latest+1 → 409 gap. Tests cover only head batch #1 | ⚠️ F-SYNC-6 (code ✅, tests thin) |
| Saturation test (60 s-latency fake LLM, rounds ≤ 20 s) | `sync.saturation_decoupled` registered, run in new-tree jobs, ASan, TSan; asserts rounds ≥ before+2 within 40 s and node 2 learns the campaign; does **not** assert a ≤ 20 s gap. Discriminates the old in-loop design | ⚠️ |

### Majority rounds (Part 1)

| Claim | Evidence | Status |
|---|---|---|
| `majority_round() = (answered+1)*2 > members`, excluding EVICTED/LEFT | formula ✅ (unit test covers 0/1..3/4); denominator = `max(db_count, members.json)` — includes evicted/left via the file | ⚠️ F-SYNC-2 |
| Gate closed after restart; opens immediately for 1 member | `sched.cpp:318` default false; fresh RoundState opens only if live lane members ≤ 1 (`757-762`, `884-890`); `EngineTest.ReservationGateStartsClosedAfterRestart`, `SingleMemberFleetReservesImmediately` | ✅ |
| 5-node partition test incl. 5 minority restarts | asserts minority own reserve frozen with 0 completed rounds (discriminates majority from any-answered), 5 restarts each frozen, majority ≤ BUDGET+3·50; loosened by `462183a`, no recorded 20/20 on the shipped version; fleet restart partly explains the freeze | ⚠️ F-SYNC-7 |

### Budget (0014)

| Claim | Evidence | Status |
|---|---|---|
| Fold ordered by (hlc, origin), capped, voided | `state.cpp:623-634, 1429-1440, 1609-1633`; `sched.cpp:736-747` | ✅ |
| One reservation per device per sync round | `sched.cpp:872-916`; engine tests + partition test | ✅ |
| Measured chunk size | `sched.cpp:899-907` = `max(50, ceil(1.2·window))`; no test above 41 attempts | ⚠️ F-BUD-2 |
| Permutation test | `StateWriterTest.CappedFoldVoidsPastBudgetInEveryDeliveryOrder` (`state_test.cpp ~848`): 6 orders of 3 batches; ranges do not overlap → tie-break untested | ⚠️ F-BUD-2 |
| 3-device 15 s-lag simulation | promised in 0014; not found anywhere (grep, `git -S`) | ❌ F-BUD-2 |

### Signing

| Claim | Evidence | Status |
|---|---|---|
| Fresh ≥128-bit nonce per request inside the signed bytes | in signed bytes (`sync.cpp:26-37`) and fresh per call, but ~32 bits entropy (F-SEC-2) | ⚠️ |
| Replay cache keyed (key, nonce) over ±60 s | key and window correct (`sync.cpp:113-114, 141-153`), run only after signature verifies; no mutex (F-SYNC-1) | ⚠️ |
| Cross-OS signing golden test | `SyncGolden.FixedKeyAndRequestGiveFixedSignatureOnEveryOS` (`sync_test.cpp:766`), runs in the 3-OS CI matrix; value is self-generated, GET with empty body only | ✅ |
| Push address recorded only after signature verifies against a current member | `sync.cpp:575-598`; `member_is_current_on`; `PushAddressRecordingRequiresACurrentMemberSignature` (unsigned, foreign key, valid) | ✅ |

### Chaos hooks and fold idempotency

| Claim | Evidence | Status |
|---|---|---|
| Hooks compile only with `ONION_CHAOS_HOOKS`; OFF in release presets | option OFF by default (`CMakeLists.txt:17`); linux-debug/windows-msvc ON, linux-release/macos-release OFF | ✅ |
| A test proves release builds ignore them | `m7.release_ignores_chaos_hooks`: behavioural (two real nodes, all four knobs set, assert alive/settle/converge; first line probes the macro); runs only in hookless jobs; disk-full hook is dead code (F-SEC-4) | ⚠️ |
| Fold idempotency | `StateWriterFoldIdempotency.DoubleAndShuffledReplayGivesIdenticalState` (`state_test.cpp:774`): doubled + one fixed shuffle, 7 batches/2 origins incl. capped RESERVE, ATTEMPT, SETTLE, CRITIQUE, ATOMIC, REVIEW; no CLAIM, MEMBER_ADD/EVICT, pools, REEXPAND; only one shuffle | ⚠️ |

### Funnel and gate as they are now

| Item | Evidence | Status |
|---|---|---|
| Thresholds | 0.7 / 0.5 / 3 attempts (F-GATE-1) | ❌ vs "0.35/0.15 shipped" |
| Critic on | unconditional (`gate.cpp:656-671`), no switch | as-is |
| Stop-rule clauses | ≥ 50 samples; last-50 atomic share ≥ 0.9 (node counts atomic if ≥ 1 verified atomic child) **and** mean score ≥ 0.8 (`sched.cpp:224-236`, `1112-1114`) | as-is |
| Critique scores the pool before synthesis | `gate.cpp:656-671` before synthesizer `:678-707` | ✅ |
| Verification on every attempt | on every non-failed attempt with an atomic label, one batched call (`:709-740`) | ⚠️ F-FUN-2 |
| Trim skipped when pool ≤ target | `gate.cpp:627` `candidates.size() <= keep` returns; no test | ✅ code / ❌ test |
| Audit sampling | does not exist | ❌ F-FUN-6 |

## A4. Test health

Environment: Linux 6.18 kali, 12 cores. Three orphaned test nodes from a
previous run were present (F-ENV-1) and were not removed.

| Run | Result |
|---|---|
| Build `linux-debug` (`cmake --build --preset linux-debug -j12`) | exit 0 (`.build-tmp/build-debug.log`) |
| Build `linux-release` | exit 0 (`.build-tmp/build-release.log`) |
| Full `ctest --preset linux-debug` (341 tests) | see "A4 results" below |

### A4 results

_Filled in below once the runs complete._

## A5. M8 exit criteria

| Criterion | Test | Status |
|---|---|---|
| A single chain under a 50% share terminates | `EngineTest.SingleChainUnderAFiftyPercentShareTerminates` (`engine_test.cpp:600`): pool 8→3→1→0, 3 settled, 3 attempts, one allowance-spent leaf at layer 3 | ✅ |
| A model that never says "atomic" terminates | `EngineTest.FakeModelThatNeverSaysAtomicStillTerminates` (`:687`): `drained`, atomic 0, attempts ≤ budget | ✅ |
| A depth checkpoint spends nothing | `EngineTest.DepthCheckpointSpendsNothingAndReleasesOnAccept` (`:637`): attempts == 1 (root's), 3 `depth checkpoint` reviews; "spends nothing" is proxied by attempts, not dollars | ⚠️ F-M8-2 |
| Compaction keeps the totals | `CheckpointTest.CompactsOldAuditRowsIntoSummaryWithoutChangingTotals` (`store_test.cpp:231`) | ✅ |
| A campaign past L9 ends within its pools | `EngineTest.FunnelCampaignPastL9EndsWithinItsPools` (`:706`): `drained`, max layer > 9, attempts ≤ budget (chain fake, pool 1024) | ✅ |
| Measured log growth within ±25% of the projection | only `PlanLogGrowth.ProjectsMeasuredBytesPerSettledNodeWithinTolerance` (`sched_test.cpp:513`) — arithmetic on synthetic numbers; the predicate is never called outside unit tests | ❌ F-M8-1 |

## A6. Security quick pass

| Item | Evidence | Status |
|---|---|---|
| Secret scan, full history | 319 commits scanned for `sk-…`, `BEGIN … PRIVATE KEY`, AKIA, `ghp_`, `xox`, `AIza`, key=value patterns: 0 hits; no `.env/.pem/.key` file names in history | ✅ |
| `.gitignore` | covers `.env`, `*.pem`, `.aws/`, `.onion_key`, `data/`; lacks `keys/`, `*.key`, `.env.*`, `*.p12` | ⚠️ F-SEC-8 |
| Bind: sync | library default 0.0.0.0, CLI default 127.0.0.1, no LAN selection or firewall code; UDP SWIM/beacon `INADDR_ANY` | ⚠️ F-SEC-5 |
| Bind: local API | hard-coded 127.0.0.1 (`api.cpp:149,154`) | ✅ |
| API token/Origin/Content-Type | enforced on POST only (`api.cpp:20-58`); `GET /api/state` and `GET /` unauthenticated; no Host check; weak token | ⚠️ F-SEC-3 |
| Key file permissions | `private_dir` chmod 0700; key file 0600; `get()` refuses group/other bits; `PlatformTest.KeyStoreRoundTripsAndRejectsTraversal` (POSIX only); Windows ACL not done | ⚠️ F-SEC-6 |
| Unauthenticated endpoints | API: `GET /api/state`, `GET /`. Sync: all authenticate except `POST /sync/v1/join` (invite-token credentialed, 64 KiB cap) | ⚠️ |
| Crypto return values | all `crypto_ed25519_check` results are checked (`sync.cpp:135,237`; `invite.cpp:87`; `log.cpp:454,597`; `beacon.cpp:61`) | ✅ |

## A7. Cost model

Inputs of `COST_MODEL.md` classified (measured / default / legacy), with the
scope of each table. Estimates are labelled as such.

| Input | Value | Class | Problem |
|---|---|---|---|
| children kept | 14 (tables A/B) | **assumed** (midpoint of 8–20) | never run end-to-end; 54.25 measured (n=4, review-accepted, old pad-to-100 config) |
| atomic share | 0.49 / 0.50 | measured on an older sample (DR 0021: 166/340, per-set 0.00–1.00) | the engine does not realise it: ATOMIC has no store effect |
| attempts/node | 1.5 (tables) / 1.75 (params) | synthetic / measured (21/12, diluted by 8/12 pre-check leaves) | assumes the gate passes early; default gate does not (F-GATE-1) |
| calls/attempt | 8 | default (assumed) | W5 baseline implies ≈ 5.9 |
| tokens in/out | 3,143/5,043 per attempt (tables) vs 570/1,128 per call (params) | two incompatible measured sources | 2-attempt run replicated 6× synthetically; excludes synthesizer + verification (F-FUN-1); W5 engine total 189 k out exceeds the proxy's total 127 k for all calls (unreconciled) |
| cache hit | 0.0 | stale measurement | V6 prefix is now default |
| prices | 0.30/0.006/1.20 peak | doc-sourced (DR 0004) | labelled as default |
| throughput | 60 k output tok/s | measured on ~365-token answers, extrapolated | funnel calls (600–1,100+ output tokens) never load-tested; single account |
| growth | 7.14 in text, 7.0 in numbers | mixed | `2,213,848 = 316,264 × 7.0` |
| leaf share | not modelled | missing | W5: 8/12 paths were cannot-split leaves |

Scope labelling: **Table A** says "P01+P06" but its 316,264 expandable nodes
include all 9 L0 roots and L0–L2 nodes the engine never auto-expands; **Table
B**'s heading says "7 empty phases" but the body is `--grow-levels 3` on all
316,264 nodes and its printed command does not reproduce it; **Table C**
("full tree to L9") uses the baseline growth 54.25 under a header naming the
0017 configuration; all three print `--data-dir ~/w5-base/data` while A/B
need `~/w54/combined` (neither is in the repo). "−51 %" compares unlike
accounting bases (FUNNEL_OPT measured V3 one-level at $7,822 > baseline
$6,786). Status: ⚠️ → regenerated in B4 (F-DOC-3).

## §2. DESIGN.md vs Accepted records (A2 detail)

DESIGN:597 token bucket 10/s vs 0011 (50; 10 rejected by measurement) ·
DESIGN:115/130/724 Keychain vs 0019 file-first · DESIGN:718-720 signing text
omits 0018 (128-bit fresh nonce, device_id binding, push-address rule) ·
DESIGN:416 omits 0014 amendment 2 (majority rounds, closed gate, 2-member
fleet needs both) · DESIGN:407 light funnel L8 vs 0017 L6 · DESIGN:473 "3 to
100 children" vs 0017 target 8–20/max 30 · DESIGN:472/485/826 omit batched
verification (0021) and keep a stale "on by default?" question · DESIGN:403
stop-rule "90% atomic" vs 0008 §2 (≥ 1 verified atomic child) ·
DESIGN:408/474 `critic_model` scope · DESIGN:302 CRITIQUE payload and :306
review reason text · DESIGN:443/487 quality guard lane weight · DESIGN:519-552
USD budget (F-BUD-1) · DESIGN:48/181 ceiling 2,000 vs 0012 (Proposed, 1,000)
· DESIGN:821-828 open questions all stale. DR 0022 vs DESIGN: consistent.
