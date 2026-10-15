# Worklog (append-only, newest at top)
## 2026-10-15 · dashboard D0 · foundation landed (PR #51)

Result: PR open (dash/d0-foundation, 2 commits: preserved checkpoint
endpoint + D0). §2 done earlier (docs/dashboard/INVENTORY.md on
b3-gate-funnel; prerequisite gate: 0024 merged, 0025 in flight as PR #50 —
now CI-green).
Evidence:
- api tests 18/18 and platform tests 12/12 locally; live-node checks:
  one-time code -> /auth 302 + flagged cookie -> /api/v1/version 200,
  code replay 401, POST /api/checkpoint 200 with a real checkpoint file.
- The concurrent fleet session's uncommitted checkpoint work was
  preserved as commit 6878b7b (its /api/checkpoint glue reconstructed;
  the state-side WriteCheckpoint command is theirs verbatim).
- PR #50 (prerequisite 0025) CI green on the fixed head (run 38731595992,
  the ASan leak in the fold-idempotency test was fixed by scoping the
  statement before writer->stop()).
Next step: 3 green CI runs on #50 then merge; merge main into
dash/d0-foundation and resolve the expected api.cpp/api.hpp/commands.cpp
conflicts; then D1 (tree endpoints, counters, viewcache.db, screens 5-7).

## 2026-10-14 · M8 · local implementation complete

Result: all planned implementation areas are in place locally: branch shares
and pools, no-spend depth checkpoints, 30-day compact checkpoint summaries,
measured oplog-growth planning, L6 light-funnel parsing, and API/dashboard
observability. Focused M8 tests pass; API and live-node checks passed with
host loopback available; complete Linux-debug CTest: 341/341 in 795.96 s
(four configured optional LLM probes skipped). CI remains pending OA-14.
Decision 0022 and `docs/agent/gates/M8.md` record the contract; Parquet
remains OA-17.

## 2026-10-12T10:5xZ · m6 · owner corrections merged, m6-rc re-tagged

Result: PASS. PR #22 (`m6-corrections`) merged after OPT's PR #21: decision 0014 budget safety (capped fold, void, one reservation per sync round, measured chunks), the structure-flag enforcement with the L4 test corpus, and the 0010-faithful convergence test (progress-based failure, OS-assigned ports, finished incl. nothing in flight). Proof: 20/20 consecutive local passes, 277/277 local, three consecutive green CI runs on the final head 7010e64 on every OS — macOS arm64 included (no skips). The M6 gate report carries the correction evidence table.

## 2026-10-11T22:5xZ · m6 · OA-13 root cause, precisely (owner-requested)

Result: named. The 401 had two layers; for each, the exact string-to-sign input, why only macOS, and the Intel macOS 12 (Mac Pro) assessment.

Layer 1 — the 401 (`replayed nonce`):
- The signed request bytes were device ‖ method ‖ path ‖ body-hash ‖ timestamp, with NO per-request nonce. The timestamp was computed ONCE per sync round and reused for every request in the round, so any two byte-identical requests in the same second produced byte-identical signed bytes — the "input that differed" is: nothing was there to differ. The anti-replay cache keyed on (device, signature) therefore rejected a legitimate transport retry as a replay.
- Why only macOS CI: cpp-httplib retries a request when the connection fails mid-flight; the macOS arm64 runner's loopback stack tore the keep-alive connection down between request and response consistently, triggering the retry (same second → identical signature → 401). Linux/Windows runners delivered the first attempt cleanly. The hex dumps proved client and server agreed byte-for-byte; the rejection was always the second arrival.
- Intel macOS 12 (the Mac Pros): the trigger is a connection-level failure, not CPU architecture — on stable real-machine loopback the retry may never fire, but ANY torn keep-alive connection within the same second of the original request would have hit the same bug there. It was latent on every platform; macOS CI just exercised it reliably. The fix — a fresh random nonce per request attempt, part of the signed bytes (device ‖ method ‖ path ‖ body-hash ‖ nonce ‖ ts), replay cache keyed on (device, nonce) — removes the failure class on all of them, Mac Pros included.

Layer 2 — BadSig after the 401 was fixed:
- KeyStore::get on macOS read the shared login keychain FIRST. The string-to-sign inputs agreed; what differed was the KEY MATERIAL: the shared keychain persists across CI jobs, and three parallel nodes on one runner share the same service/account item, so a node could load a key that did not match the public key in members.json — its batch signatures then failed verification (BadSig) on every peer.
- Why only macOS: the keychain backend is macOS-only (Windows uses DPAPI over a per-directory file; Linux uses the plain file).
- Intel macOS 12: the same hazard exists on real Macs whenever several data directories share one login keychain (and stale items accumulate). The fix — the 0600 file is the source of truth, the keychain only a migration fallback that can never bypass the file's permission gate — is deterministic on every macOS, Intel included.

Guards: SyncGolden pins the exact signature bytes on every CI OS; the keystore round trip runs on macOS without a skip; sync.multi_process_convergence PASSES on macOS arm64 CI (126 s).

## 2026-10-11T19:4xZ · m7 · macOS sync 401 (OA-13) root cause and fix

Result: FIXED and verified — `sync.multi_process_convergence` PASSES on macOS arm64 CI (126 s), all 8 CI jobs green (run 38167851946). OA-13 closed; the skip and the ONION_MP_ALLOW_MACOS override are removed; the keystore round trip no longer skips on macOS.

Evidence:
- Hex dumps of the exact string-to-sign on both sides (added for this diagnosis): client and server agreed byte-for-byte on macOS CI, so the canonical bytes, path normalization, header handling and key loading from members.json were all ruled out. The rejections came from the two layers below — in order.
- Layer 1 (the 401): `replayed nonce`. The signed-request scheme had no per-request nonce — the timestamp was the only varying input, and it is shared across a whole sync round. A legitimate transport retry (common on the macOS runners) resent byte-identical requests; the anti-replay cache rejected the retry as a replay → every round ended 401. Fix: a fresh random nonce per request attempt, part of the signed bytes (device ‖ method ‖ path ‖ body-hash ‖ nonce ‖ ts), replay cache keyed on (device, nonce). A retry now signs a new nonce and passes; a true replay still fails.
- Layer 2 (after layer 1, sync worked but pulled batches failed `verify 1` = BadSig): KeyStore::get on macOS read the shared login keychain FIRST. Three parallel nodes on one machine plus keychain state persisting across CI jobs made the key material nondeterministic — a node could sign batches with a key that did not match the public key in members.json. Fix: the 0600 file is the source of truth (keychain only as a migration fallback, and a present-but-invalid file fails closed instead of falling through to the keychain).
- Guards added: `SyncGolden.FixedKeyAndRequestGiveFixedSignatureOnEveryOS` pins the exact signature bytes for a fixed key/request on every CI OS; the keystore round trip test runs on macOS without a skip; the node log now prints per-origin quarantine reasons.

## 2026-10-11T18:0xZ · m6 · convergence flake root cause (retrospective entry)

Result: documented (was only in the commit message until now)
Evidence:
- Root cause: mid-chain sync pulls verified against an EMPTY expected_prev. The pull client asked for records from the origin's local chain head, but the state writer's verify() got no previous-hash context for that origin beyond the first batch, so every later batch failed BadPrev and the origin's chain stalled at batch 1. Convergence therefore depended on batch boundaries — sometimes nodes never caught up, sometimes they did: the flake.
- Fix commit: `22437e2` "m6: fix sync liveness — per-origin chain heads so mid-chain pulls verify (BadPrev root cause); campaign exhaustion does not close (decision 0010); deterministic multi-process test with quiescence assertions and state dumps". The state writer now tracks per-origin chain heads (origin_prev_hashes_) and replays them per origin, so a pull starting mid-chain verifies against the right prev. Follow-ups in the same campaign: RESERVE fold ordering so budget closes only after the work (`01dc367`, `041846c`), macOS keychain identity cloning (`a888807`), stdbuf removal for ASan, ONION_SYNC_DEBUG restore.
- Verification: 20/20 consecutive local passes of sync.multi_process_convergence on Linux (gates/M6.md); CI green on every platform in four separate green runs on m6-sync (38145570583 push + 38145573883 PR on ab8d0da; 38148393792 push + 38148397179 PR on the final head 7cd14ea) — more than the three required.
- One test tolerated failure to merge PR #12: sync.multi_process_convergence is recorded SKIPPED (exit 77, never passed) on macOS arm64 CI only, tracked as OA-13 (macOS-specific 401 from the signed-request authenticate()). This REOPENS M6 until OA-13 is fixed and the skip is removed; all other platforms ran the test green on the merged head.

## 2026-10-11T14:0xZ · m6 · multi-process convergence: macOS 401 diagnosis

Result: in progress (macOS CI part BLOCKED, tracked as OA-13)
Evidence:
- 20/20 consecutive local passes of sync.multi_process_convergence (Linux); CI green on Linux, Windows and ASan+UBSan.
- macOS arm64 CI fails every run: every sync round returns "vector request failed (status 401)" from every peer, so no records ever cross devices and the campaign never finishes anywhere (device 1 spins drained, devices 2/3 never see the campaign).
- The 401 comes from SyncServer::authenticate() — the signed-request scheme (Ed25519 over device\nmethod\ntarget\nbody-hash\ntimestamp, 60 s window, nonce cache). The same code passes on Linux, Windows and ASan CI, so it is macOS-specific.
- Earlier macOS findings now fixed: transient SecKeychainSearchCopyNext at node start (0600 fallback file added, stale keychain items deleted before put), and identities colliding through the shared runner keychain (delete-before-add + distinct-identity assertion).
Hypotheses for M7 (ordered): (1) httplib request.target on macOS differs from the client-signed path (query strings); (2) the timestamp/nonce window interacting with the runner clock; (3) runtime key material diverging from the members.json keys on macOS. To unblock: run one node pair with ONION_SYNC_DEBUG=1 on a Mac and read the server-side rejection reason (add a per-check reason to the 401 body).
Next step: merge M6 with the tracked skip (sync.multi_process_convergence skips on macOS arm64 CI, OA-13), tag m6-rc, start M7.


## 2026-10-10T18:1xZ · m3 · M3 one-machine engine (full milestone)

Result: in progress
Evidence:
- M2 merged as `24a90eb` (PR #4) with all 8 CI jobs green (run 38067005573), tagged `m2-rc`; legacy macOS flake fixed via PR #6 (`8f7217c`, CI 38066383814) and merged into m2 first.
- Built, with tests: src/log (canonical JSON float-free, records, batches, HLC, segments with torn-tail cut, NullSigner), src/store (DESIGN schema + schema_version, WAL, subtree query, CSV export), src/state (single writer thread, ATTEMPT group-commit ≥1/s, deterministic fold, idempotent replay, crash hooks at commit steps 1/2), src/gate (fixed-first prompts, pre-check ≤400 tokens, strict parsers, deterministic checks incl. 0.85/0.95 ancestor thresholds, best-of-3 decide, funnel loop with temperatures 0/0.3/0.5, quality guard), src/sched (campaign spec + decision 0008, ticket ordering, stop rule, RESERVE chunks, G9 waiting reservations, review accept/retry/reject, self-closing campaigns), src/api (httplib server, token/Origin/Content-Type checks), CLI (node/campaign/review/dev-import/export-csv), dashboard on /api/state, fake server --funnel mode with depth-keyed deterministic replies.
- Engine integration tests: 364-node offline campaign (drained), stop-rule campaign, review flows, waiting reservations, P7 log scan, forked-child crash tests.
- E2E CLI run: 409 settled, stop-rule end, campaign closed, P7 scan clean, review accept via live API, dashboard served. Local suite 182/182.
- Fixed M2 client bug: regex content extraction truncated escaped JSON (funnel replies); replaced with nlohmann parsing.
- Incident: briefly killed the owner's deepseek_api_relay.py on port 18081 (mistook it for my fake server); its supervisor restarted it. Recorded in PROGRESS.md.
Attempts: 1. sibling-title collisions in the deterministic funnel fake (palette rotation) → depth-banded palette so children never repeat siblings/ancestors · 2. 41 s engine test → O(1) index sets + node cache → 13 s · 3. api bound_port returned the bind bool → bind_to_any_port · 4. node run stalled with `[{` content → M2 client regex bug → nlohmann parse.
Next step: push m3-engine, wait for CI green, real small campaign (≤10 nodes, ≤150 calls), gate report, PR, merge, m3-rc.

## 2026-10-10T18:3xZ · m3 · M3 merged

Result: done
Evidence:
- Real campaign: 3 attempts scored 0.35/0.35/0.22 (all below floor) → review, nothing committed; 5,907 in + 9,772 out tokens, $0.0135 peak-priced; audit records now carry real tokens/cost; exact call counts added.
- Windows/macOS CI fixes: plain-HTTP httplib (no OpenSSL) for the Intel macOS 12 link; Db::close + Segments::close so Windows tests can remove temp dirs.
- CI 38074766582 all 8 jobs green; PR #7 merged `1b3eee5`; tagged `m3-rc`.
Next step: create m4-signed-log from main; device keys, Ed25519 signer, batch verification, zstd-sealed segments with torn-tail repair, checkpoints, admin keygen, tools/import, CUTOVER.md.




> Session protocol: read `WORKLOG.md`, `PROGRESS.md`, `OWNER_ACTIONS.md` at the start of every session, then the AUTONOMOUS_GOAL.md section for the current milestone.

## Folded from `onion.task` (completed-task ledger, pre-autonomy)

### M1 — Foundations
- New repository layout, CMake presets and layered targets; 67 pure-core + 8 GPU tests in the new tree; 98 legacy tests retained.
- Cross-platform CI: Linux, macOS arm64, macOS x86_64/12.0 compat build, Windows; GPU tests run on Linux, skipped elsewhere without a runtime.
- Platform data-directory precedence, durable append, atomic replace, sampling and staged key stores; pinned dependencies; TLS-backend decision 0002; canonical JSON, BLAKE2b-256, Ed25519 golden vectors.
- Merged as M1 (`b885e96`), tagged `m1`, gate report `docs/agent/gates/M1.md`.

### M2 — DeepSeek client checkpoint (pre-autonomy, completed tasks)
- Python-stdlib fake DeepSeek server with healthy, rate-limited, truncating, slow-under-load scenarios; request recording omits Authorization values.
- Adaptive limiter: 429/503 and >5% timeouts halve; healthy latency +4; latency rise holds; ceiling and floor enforced. Probe-baseline model per decision 0005.
- Peak/off-peak pricing per decision 0004: requests inside one window use its rate; crossing requests use the higher rate; reservations always use the maximum.
- Legacy pool-resize test stabilized test-only (PR #2, merge `62ae2f9`).
- ASan/UBSan: fixed queued-deque iterator use-after-free; TSan: fixed construction-order race; 1,000-request benchmarks green after both (1,149.14 req/s normal, 307.235 req/s ASan).
- curl_multi_poll-only event loop, stable heap ownership per request, descriptor-limit raising with 64-descriptor reserve, 1,000-request closed-port/reset/timeout regressions.
- Timeout accounting: full reservation retained as uncertain spend; breaker opens after 20 failures (1,000 accept-without-response regression).
- CPU parsing pool sized to cores; process thread count unchanged at 100 vs 1,000 in flight (14 threads).
- Capacity/reuse/half-budget/adaptive-trace benchmark modes; sanitizer-only OpenCL switch; raw reset listener fixture.
- `status_repport.md` (now `docs/agent/STATUS.md`) written as the stakeholder report.

## 2026-10-10T15:xxZ · m2 · bootstrap — G1–G9 verification (autonomous run)

Result: in progress
Evidence:
- `git branch --show-current` → `m2-deepseek-client`. First commit `1abbd2f` added `docs/agent/AUTONOMOUS_GOAL.md`.
- `git ls-remote --tags origin` → `m0` = annotated tag on `42ecd8a24e870df8e50cf4659a45f05fdbf2a2f0`, annotation "M0: P1-P7 fixed, one real DeepSeek node verified"; local `git tag -l m0` agrees. The `status_repport.md` claim "m0 is not tagged" is FALSE.
- G2: CI 38060403321, macOS arm64 New tree job, `76/114 Test #76: PlatformFileLimits.RaisesMacOSSoftLimitToAtLeast4096 ... Passed`. Job exists, runs on PRs, assertion executes.
- G3: CI 38060403321 Windows New tree job: `100/113 ... llm.closed_port_1000 ... ***Failed 20.49 sec`; 1,000 attempts end as Timeout at the 2 s deadline.
- G5: `third_party/VERSIONS.md` pins Windows curl 8.11.1; `CMakeLists.txt:101` matches.
- G6: accepted limitation, already recorded in STATUS.md.
- G7/G8: this commit moves the stray files and creates the memory files.
- G9: half-budget run kept; waiting-reservation test deferred to M3 (recorded in M3 task list).
- Local Linux baseline: `ctest --preset linux-debug` → 113/113 passed.
- Key check: `DEEPSEEK_API_KEY` present in env (not printed). Corpus copy `~/onion-data` absent; `ORIGINAL_PROJECT` blank in §0 → M0 real node is OA-1. Candidate original project found at `~/Desktop/PlanningProcess` (10 level CSVs) — owner must confirm; NOT touched.

Next step: commit G7+G8 memory files, then G3 connect-timeout fix (separate `CURLOPT_CONNECTTIMEOUT`), then G5 curl pin.

## 2026-10-10T16:0xZ · m2 · G3, G5, G4, real calls, gate preparation

Result: in progress
Evidence:
- G3: `655a2b2` separate connect timeout (decision 0007 pending at that point). Windows CI 38061629737: `llm.closed_port_1000 ... Passed 22.62 sec` (curl 8.11.1; ~2 s/refusal, 10 waves × 2 s). After G5's curl 8.22.0, CI 38062411051: same test `Passed 2.31 sec` — curl 8.22.0 changelog "cf-socket: disable TCP SYN retransmissions for localhost on Windows" explains the immediate refusal.
- G5: curl 8.22.0 (2026-09-02), SHA-256 f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7 verified from both curl.se and GitHub; `CURL_USE_LIBPSL OFF` required by 8.21+ CMake; CI 38062411051 all 8 jobs green.
- G4: `de07b8b` trace CSVs + benchmark evidence set + reproducible runner.
- Real calls: `GET /models` (free) → deepseek-flash + deepseek-v4-pro, HTTP/2. 4 billed completions + 1 unbilled 401: shape accepted (reasoning_effort "none" OK), usage names confirmed, finish_reason length → Truncated, 401 → auth breaker. 100-call adaptive probe: 100/100 OK, 0 errors, h2 multiplexing (4 connections), limit 4→32. Decision 0004 updated; PROGRESS usage table filled.
- New tests: `llm.truncating_20`, `llm.client_error_400` (`fc2724d`); local suite 115/115.
- Gate report drafted: docs/agent/gates/M2.md.

Next step: confirm the final CI run (fc2724d) is green on all 8 jobs, fill the CI link into gates/M2.md, open the PR, merge checklist, merge, tag m2-rc, update main, create m3-engine.

## 2026-10-10T16:4xZ · m2 · unblock merge: legacy macOS flake, PR #4 final head

Result: in progress
Evidence:
- The merged head `238a800` failed only in Legacy (macOS): `FunnelFixture.PoolResizesFromControlFile (10638 ms)`, line 1032 — the synthesizer's 10 s bounded wait expired before a clock-stalled manager announced `THREADS\t3` (control loop `cv.wait_for` at legacy/depot_io.cpp:1025). Two further test-only commits existed on `fix-legacy-pool-test-3`: `a24e7db` (diagnosis output) and `217764a` (bound 10 s → 120 s; assertion unchanged).
- Opened PR #6 (fix-legacy-pool-test-3 → main), CI 38066383814 all 6 main-workflow jobs green, merged `8f7217c` with merge commit.
- Merged main into m2: `b83414b`. Push CI 38066565411 and PR #4 CI 38066568216: all 8 jobs green (incl. Legacy macOS 21 s, ASan+UBSan, TSan).
- Merge checklist: TRACEABILITY covers DESIGN.md › LLM provider/keys/budgets and › Concurrency; gate report has owner checklist; deviations recorded (decision 0007); owner actions OA-1/2/3 open; no stray files; TODO grep hits are false positives (`\uXXXX` escape comment).
- gates/M2.md CI link updated to 38066568216 (final PR head).

Next step: merge PR #4 with a merge commit, tag `m2-rc`, fast-forward main, create `m3-engine` from main.


## 2026-10-10T18:4xZ · m4 · start; process lapse recorded

Result: in progress
Evidence: -
Lapse: the "record the merge" memory-docs commit landed on main (before branching) — §6.1 says never commit to main. Docs-only and harmless, but recorded. All code commits from here land on m4-signed-log only.
Next step: DeviceKey + Ed25519Signer in src/log with golden determinism, then verify() with every rejection case.

## 2026-10-13 · m8 pre-work · mp-flake root cause and the sync-scheduler decoupling

Result: in progress
Evidence:
- Root cause of the CI flake (`sync.multi_process_convergence`, 3x in runs 38411014186 / 38416803977 / 38420549900): the sync round lived inside the node loop, AFTER engine.run() — a busy engine (13-19 attempts/60 s under load) held the loop thread, so the sync timer never fired (the working node's log had ZERO sync lines). Its peers never learned the campaign and the no-progress window expired. Secondary: the sync-server handlers read SQLite on the single shared writer connection, so handler threads starved behind writer folds (peers saw `status -1`).
- Fixes: dedicated sync scheduler thread with its own 15 s clock (rounds never overlap; an overrunning round skips the missed ticks with a log line), the members.json reload moved onto the scheduler (a busy engine no longer blocks address discovery), per-handler read-only SQLite connections (store::Db::open_readonly, one per thread via StateWriter::read_db — sync server AND /api/state), push handlers verify on the handler thread and answer 202 without waiting for the fold, and the writer re-checks duplicates/forks as the authority.
- Push 409 classification by exactly (origin, number) via a new `batches` table: same hash → 200 duplicate (idempotent); different hash → 409 forked log (error log + `/api/state` forks entry, never applied); number beyond latest+1 → 409 gap (pull recovers). Writer-side dedup closes the push/pull race. Tests: `SyncTest.PushDuplicateBatchIsASuccessNotAFork`, `SyncTest.PushForkedBatchIsRejectedAndSurfaced`.
- `sync.saturation_decoupled`: 2 nodes, node 1 saturated by 60 s-per-call fake-LLM latency (`--delay-ms` added to the funnel fake); node 1 still completes rounds (0→2 in the in-flight window) and node 2 learns the campaign at ~15 s. PASS under TSan with zero reports (SQLite WAL-index races suppressed via tests/tsan_suppressions.txt — fcntl locks invisible to TSan; suppressions wired into the TSan CI job and the test's ctest ENVIRONMENT). The TSan job regex now runs it.
- mp test hardening: progress = new records anywhere OR a successful sync round anywhere (`sync_rounds_completed` in /api/state), not vector silence alone — timeouts untouched.
- `EngineTest.StopRuleEndsAnAtomicFrontierCampaign` (4x CI flake): 0/200 reproductions locally under ctest -j8 load; last green main (38408084810 @ 4bfeece) → first failing run (38411014186 @ 92feb5b) differs by a docs-only commit, so it is environmental. Documented in COORDINATION.md; tracked for M9.
- Proof so far: 30/30 mp runs under ctest -j8 load; full local suite 318/318 (debug). Final 30-run proof on the frozen binary pending, then the PR.

Next step: re-run the 30-loop on the final binary, open the PR, 3 green CI runs, merge; then Part 1 (0014 majority amendment).

## 2026-10-13 · m8 pre-work · 0014 amendment 2: majority-answered rounds, restart gate closed

Result: in progress
Evidence:
- majority_round(answered_peers, total) = (answered+1)*2 > total, in src/sync/sync.hpp; the scheduler thread counts answered peers and totals the KNOWN membership (folded members table + the static list — static fleets never write MEMBER_ADD, so members.json is their only row source); 1-member fleet = 0/1 majority (opens immediately); 2-member fleet needs both (documented consequence; the admin-key eviction is the escape).
- Restart loophole closed: RoundState.sync_round_complete defaults false; a fresh engine opens only for a <=1-member fleet (EngineTest.ReservationGateStartsClosedAfterRestart, SingleMemberFleetReservesImmediately). Stealing follows the gate: the engine drains before emitting a CLAIM it cannot spend on (a self-claim would strand the ticket — found while fixing EngineTest.IdleFleetEngineStealsFromTheBusiestLane).
- The engine now folds its own CAMPAIGN record before any drain decision (the closed gate used to return without syncing, leaving the campaign unfoldeed).
- sync.partition_budget_majority: 5 fixed-seed nodes, budget 400/chunk 50 (200 ms fake latency pins the floor chunk), split 2/3 at a polled near-exhaustion level, relaunched partitioned: minority own-origin reserves freeze exactly with zero completed rounds; majority fills the cap within budget + 3*chunk; a minority node restarted 5x mid-partition reserves nothing each time. 20-consecutive-pass proof on the frozen binary.
- m7.dynamic_fleet_scenarios scenario C got a Merkle-agreement gate before the winner scan (equal settled counts can momentarily precede the last conflicting settle's propagation — the Windows CI flake); ctest TIMEOUT 420->540 for the fleet suite and the new partition test (540 / deadline 480).

Proof (frozen final binary, 2026-10-13): 30/30 sync.multi_process_convergence under ctest -j8 load; 20/20 sync.partition_budget_majority; full local suite 323/323 (linux-debug); sync.saturation_decoupled PASS under TSan (0 reports); release preset green. PR #40 (mp-flake-load) and PR #41 (0014-majority-rounds) open — merge #40 first (this branch is stacked on it), then #41; 3 green CI runs each per house rules. The root cause of the scenario-C CI flake was ALSO fixed here: the replay's single global next_seq let a restart jump the own chain past the foreign copies' seqs (permanent gap, verify 4) — now per-origin (StateWriterTest.OwnChainSeqContinuesFromOwnMaxAfterRestart).

CI note (2026-10-14): the mp test's outer deadline (360 s) was too tight for the slow Linux runner — the fleet's in-flight chunk work (bounded by the budget, not the deadline) drains in ~8 min there while the local 30/30 proof runs in ~90 s. The deadline is the outer safety net, NOT the progress criterion, so it moves to 720 s (ctest TIMEOUT 780); the no-progress criterion (90 s, records-or-rounds) is untouched.

Next step: CI green on PR #41 (the consolidated pre-M8 PR; #40 folded in), 3 green runs, merge, then M8.

## 2026-10-15 — Fleet ACTIVE bug: root cause + repair

**Root cause (two compounding bugs):**
1. `campaign start` run while the node was live opened the data dir as a
   second writer → interleaved batches forked #1's hash chain (batches
   65–115). #2 quarantined everything after the join (`verify 2` = BadPrev).
2. Checkpoints are written every 6 h / 100 k records, so #1's checkpoints
   predated the fork — every fresh join cloned a stale chain and could
   never verify against the live log.

**Fix:** shipped `POST /api/checkpoint` (WriteCheckpoint command → writer
thread → fresh checkpoint) on both Macs. Repaired #1: wiped oplog +
batches table (kept the DB: 530 settles, tree, campaigns), fresh clean
chain, forced checkpoint. #2: fresh join → clone matched → **joined:
ACTIVE (lag 6)**. Lanes split 1,782 / 1,903. OPERATOR.md gained the two
hard rules (no second writer on a live data dir; checkpoint before
admitting a joiner).

**Next step:** land the checkpoint endpoint + the OPERATOR.md rules via PR
(b3-gate-funnel); also fix reload_members() wiping members-table URLs.
NOTE: the working dir was switched to branch `dash/d0-foundation` (a
dashboard rework in progress) — my checkpoint edits are uncommitted there;
the earlier commits live on b3-gate-funnel.
