# Traceability — DESIGN.md rules → tests

Every rule touched by a milestone must have at least one passing test before that milestone merges. Status: ✅ passing in CI · ⏳ pending · 🔵 owner live check.

## M2 scope: DESIGN.md › LLM provider, keys and budgets

| Rule | Test(s) | Status |
|---|---|---|
| Budget: reservations prevent overspend with concurrency | `Budget.ConcurrentReservationsNeverOverspend` (500 threads, 100 accepted) | ✅ |
| Budget: unused reservation released on settle | `Budget.SettlementReleasesUnusedReservation` | ✅ |
| Budget: timeout keeps full reservation as uncertain spend | `Budget.TimeoutKeepsFullReservationAsUncertainSpend`, `llm.timeout_1000` (`timeouts_uncertain_billed=1000`) | ✅ |
| Metering: usage block cache-hit/cache-miss/output × peak/off-peak; crossing windows billed higher | `Metering.UsesOffpeakOnlyForCallsInsideOneWindow`; decision 0004 | ✅ |
| Errors 429/503: retry with backoff/Retry-After, halve limiter, no breaker count (429) / count (503) | `Limiter.HalvesOnRateLimitAndHoldsOneWindow`, `Limiter.HalvesOnServerError`, `CircuitBreaker.TimeoutsCountAndRateLimitsDoNot` | ✅ |
| Timeout: counts toward breaker, contributes to 5% limiter rule | `CircuitBreaker.TimeoutsCountAndRateLimitsDoNot`, `Limiter.HalvesOnlyWhenTimeoutsExceedFivePercent`, `llm.timeout_1000` | ✅ |
| 401/402: no retry, breaker opens after 3 with reason | `CircuitBreaker.AuthAndBalanceOpenAfterThreeWithReason`, `llm.auth_401`, `llm.balance_402` | ✅ |
| Transport failure: counts toward 20-failure breaker; refused connection is Transport, never Timeout | `CircuitBreaker.OpensAfterTwentyConsecutiveTransportFailures`, `llm.closed_port_1000`, `llm.reset_connections_1000` | ✅ |
| Successful/truncated response resets the breaker | `CircuitBreaker.OpensAfterTwentyConsecutiveTransportFailures` (Ok resets) | ✅ |
| `finish_reason: "length"` classified as failed attempt (Truncated) | `llm.truncating_20` (20 truncated replies: all failed, breaker stays closed) + real-API observation in decision 0004 | ✅ |
| Prompt caching: fixed text first, node text last | client payload order; real repeat produced 0 hits on a 17-token prefix (finding in decision 0004) | ✅ recorded |
| Reasoning knob `reasoning_effort: "none"` on every request | client payload construction; accepted by the real API (decision 0004 update) | ✅ |
| Keys: never in logs, records, or traffic | `fake_deepseek_test.py` (Authorization absent from recorded request) | ✅ |
| 400/422: no retry, count toward breaker | `llm.client_error_400` (20 × 400 opens the provider-or-transport breaker) | ✅ |

## M2 scope: DESIGN.md › Concurrency

| Rule | Test(s) | Status |
|---|---|---|
| Adaptive limit adjusted per 30 s window; halve on 429/503 or >5% timeouts; hold one window | `Limiter.HalvesOnRateLimitAndHoldsOneWindow`, `Limiter.HalvesOnServerError`, `Limiter.HalvesOnlyWhenTimeoutsExceedFivePercent` | ✅ |
| Grow +4 at ≤1.5× low-load baseline | `Limiter.GrowsByFourAtHealthyLatency` | ✅ |
| Hold when latency rises without errors | `Limiter.HoldsWhenLatencyRisesWithoutErrors` | ✅ |
| Never exceed profile ceiling; floor of 1 | `Limiter.NeverExceedsCeiling`, `Limiter.FreshLimiterReportsClampedProfileStartingValue` | ✅ |
| Hourly probe: cap at max(start, limit/4), record baseline, restore limit; busy windows never update baseline | `Limiter.ProbeCapsRestoresAndBusyWindowsDoNotDriftBaseline`, `Limiter.GrowsByFourAtHealthyLatency` (begin_window(3600)), `Limiter.LoadCurveConvergesAcrossSixSimulatedHours` | ✅ |
| 1,000 requests in flight on ~20 threads (libcurl multi + CPU pool, no thread per call) | `llm.fake_server_load_1000` (`fake_deepseek_load_test.py`), bench `process_threads=` evidence | ✅ |
| Waiting and working separated; worker = task slot | CPU parsing pool + curl_multi event loop | ✅ |
| FD ceiling caps in-flight requests | `PlatformFileLimits.ReservesDescriptorsForEngineResources`, `PlatformFileLimits.RaisesMacOSSoftLimitToAtLeast4096` (macOS CI) | ✅ |
| G6: Windows vendored curl has no HTTP/2 → HTTP/1.1 fallback | accepted limitation (owner decision), recorded in STATUS.md / M2 gate | ✅ recorded |
| Separate connect timeout from total timeout (slow Windows refusal classified as Transport) | `llm.closed_port_1000` with `--connect-timeout` (decision 0007); Windows CI passed in 2.31 s | ✅ |

## M1 scope (kept from M1 gate)

- Golden vectors and platform rules: see `docs/agent/gates/M1.md`.

## M3 scope: DESIGN.md › Storage, Op log, Campaigns, Work scheduling, Quality gate, Depth, Concurrency

| Rule | Test(s) | Status |
|---|---|---|
| SQLite schema exactly as DESIGN.md › Storage + schema_version | `StoreTest.*` (schema, WAL, subtree, CSV export, newer-schema refusal) | ✅ |
| Subtree query: `T01` never matches `T010` | `StoreTest.SubtreeQueryNeverCrossesSiblingPrefixes` | ✅ |
| One JSON line per batch, LF only, one durable write per batch | `SegmentsTest.*`, `StateWriterTest.TerminalBatchCommitsDurablyAndFolds` | ✅ |
| Canonical bytes: sorted keys, no whitespace, exact int64 decimal, no floats | `CanonicalJson.*`, `Records.CanonicalBytesAreStable` | ✅ |
| HLC = (ms << 16) \| counter, monotonic | `HlcClock.IsStrictlyMonotonic` | ✅ |
| Records never contain floats | `CanonicalJson.RejectsFloats` (throws) | ✅ |
| ATTEMPT group-commit, flush ≥ 1/s (decision 0000) | `StateWriterTest.AttemptsGroupCommitIntoOneBatch` | ✅ |
| Terminal batches carry children, CRITIQUE, ATOMIC, SETTLE last — nothing committed before the gate decision (P7) | `EngineTest.RunsAThreeHundredNodeCampaignOffline` (log scan: no failed attempt for any settled node), `GateDecide.*` | ✅ |
| Pre-check ≤ 400 tokens; a clear no makes a leaf without spending an attempt | `GateFunnel.PreCheckNoMakesALeafWithoutSpendingAnAttempt`, `GatePrompts.PrecheckPromptIsAtMostFourHundredTokensApprox` | ✅ |
| Checks: complete reply, valid JSON, 3–100 children, no repeats (node/ancestor/sibling/existing), style gates | `GateChecks.*`, `GateParsers.*`, `GateFunnel.TruncatedReplyIsAFailedAttempt` | ✅ |
| Ancestor check: parent ≥ 0.85, higher ancestors ≥ 0.95 | `GateChecks.AncestorSimilarityThresholds` | ✅ |
| Best-of-3: commit ≥ 0.7, retry with issues + raised temperature, leaf when no 3 valid children, floor 0.5, NEEDS_REVIEW below floor | `GateDecide.*`, `GateFunnel.RetryInjectsPreviousIssuesAndCommitsBetterAttempt`, `GateFunnel.NeedsReviewBelowFloorSpendsNothingMore` | ✅ |
| Temperatures 0 → 0.3 → 0.5 | `GateFunnel.RetryInjectsPreviousIssuesAndCommitsBetterAttempt` | ✅ |
| Quality guard flags outliers | `QualityGuard.*` | ✅ |
| Campaigns: spec file, start/pause/resume/stop, stop is final | `ParseCampaign.*`, `StateWriterTest.CampaignFoldAndStopIsFinal`, `EngineTest.StopIsFinal` | ✅ |
| Cost projection before start | `ProjectCost.ScalesLinearlyInExactDecimals` + CLI evidence | ✅ |
| Ticket ordering: breadth/deepest first, ties by path; scope, settled, leaf and review exclusions; review pauses the subtree | `OrderTickets.*` | ✅ |
| Loop budget in RESERVE chunks (default 50) | `EngineTest.*` (RESERVE records in the log of every campaign run) | ✅ |
| Stop rule: last 50 settled ≥ 90% atomic and average score ≥ 0.8 | `StopRule.*`, `EngineTest.StopRuleEndsAnAtomicFrontierCampaign` | ✅ |
| Waiting reservations: a ticket that does not fit waits and starts after a release (G9) | `EngineTest.WaitingTicketStartsWhenTheReservationIsReleased` | ✅ |
| Review queue: accept settles without spending, retry re-queues, reject tombstones (decision 0008) | `EngineTest.ReviewAcceptSettlesWithoutSpending`, `EngineTest.ReviewRejectTombstonesTheSubtree` | ✅ |
| Kill at every commit step loses no committed work | `StateWriterTest.CrashBetweenDurableWriteAndFoldLosesNothing`, `StateWriterTest.CrashAfterFoldKeepsStoreAndLogConsistent` (forked child, real `_exit`) | ✅ |
| API: token, Origin, Content-Type checks on every POST; 127.0.0.1 only | `ApiTest.*` (401 / 403 / 415 / 200) | ✅ |
| Dashboard reads /api/state with campaign panel and review buttons | `ApiTest.DashboardServesAtRoot` + live `/api/state` evidence | ✅ |
| 1,000 requests in flight on ~20 threads (M2 carry-over) | M2 tests, still green | ✅ |

## M4 scope: DESIGN.md › Device identity and ID slots, Op log format, Joining, Membership, Storage (checkpoints)

| Rule | Test(s) | Status |
|---|---|---|
| Device key generated on first start; device_id = first 16 hex of BLAKE2b-256(pubkey) | `DeviceKey.DeterministicIdentityFromSeed`, CLI key-store flow (`genesis_import.verify` needs the stored key) | ✅ |
| Slot = top 20 bits; node ids = slot × 2^40 + counter, `TK%05lld` | `NodeIdAllocator.AllocatesFromSlotTimesTwoToTheForty`, `NodeIdAllocator.SlotBounds` | ✅ |
| Batch 1 chains from BLAKE2b-256(public key); one signature per batch over prev ‖ hash | `SignedWriterChainsFromThePublicKeyHash`, `VerifyTest.ChainAcrossBatches` | ✅ |
| Signer matches the M1 golden Monocypher signature (seed 00..1f) | `Ed25519Signer.MatchesMonocypherGoldenSignature` | ✅ |
| Verification: bad signature, bad prev, bad hash, gap, non-member, past cutoff, unknown version — quarantined, never applied | `VerifyTest.*` (one test per rejection) | ✅ |
| Torn tail: a cut at every byte offset of the last batch keeps exactly the complete batches | `StateWriterTest.TornTailAtEveryByteOffsetKeepsCompleteBatches` | ✅ |
| Sealed segments (zstd) replay across restarts | `StateWriterTest.SealedSegmentsReplayAcrossRestart`, `SegmentsTest.RotatesWhenSealBytesWouldBeCrossed` | ✅ |
| Checkpoints: VACUUM INTO, signed manifest with vector and Merkle root, keep the last 3 | `CheckpointTest.*`, decision 0009 | ✅ |
| Admin keygen refuses the data directory, prints the public key | CLI evidence (gate report), `admin keygen` exit codes | ✅ |
| Genesis import: duplicates need an approved resolution; counts per layer, parents, Merkle root verified | `genesis_import.verify` (fixture corpus with duplicates) | ✅ |
| A lone eviction signature is rejected; an admin-signed one is accepted | `EvictionValid.LoneMemberSignatureIsRejected` | ✅ |

## M5 scope: DESIGN.md › Member profiles, LLM provider (keys), Concurrency (safety caps)

| Rule | Test(s) | Status |
|---|---|---|
| Profile scan measures OS, CPU, RAM, disk, durable write, GPU fp64, battery, link speed, RTT — unknown never invented | `node profile` live output; platform `os_name/total_ram_gb/disk_free_gb/link_speed_mbps` | ✅ |
| Rules version 1: desktop 1.0/16/2000/2GB, laptop 0.25/4/32/8/1GB | `ProfileRules.DesktopAndLaptopDefaults` | ✅ |
| node.conf overrides always win | `ProfileDerive.OverridesAlwaysWin`, `ProfileNodeConf.ParsesOverridesAndComments` | ✅ |
| MEMBER_PROFILE only when something changed; version, prev, reason, changed | `ProfileVersions.NewProfileOnlyOnChange`, `ProfileVersions.OverrideChangeBumpsVersion` | ✅ |
| Records never contain floats: profile measurements are decimal strings | profile spec construction (decision 0003), all profile tests | ✅ |
| Key storage: Keychain (macOS), DPAPI (Windows), 0600 file (Linux); keys never in service definitions | platform KeyStore backends; `ServiceFiles.GoldenContent` (no Environment=, no key text) | ✅ |
| Service definitions: systemd Restart=always + StateDirectory, launchd KeepAlive, Windows restart-on-failure | `ServiceFiles.GoldenContent`, `ServiceFiles.EscapeXmlInPlist` | ✅ |
| Profile feeds the limiter start value and ceiling | `node run` derives the client limit from the profile | ✅ |
