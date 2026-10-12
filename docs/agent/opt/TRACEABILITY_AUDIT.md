# TRACEABILITY_AUDIT — DESIGN.md rules vs tests (OPT track, W3)

Covers the DESIGN.md sections the OPT track owns (§2.3 of the goal): LLM
provider/keys/budgets, Concurrency, Quality gate parameters, and Cost. Every
rule maps to at least one test or a labelled gap. Status: PASS = passing
test on every OS in ci.yml; NIGHTLY = passing test in opt-nightly.yml;
GAP = missing, with the owner workstream.

## LLM provider, keys and budgets

| DESIGN rule | Test(s) | Status |
|---|---|---|
| Provider configured explicitly, per device | `ProfileNodeConf.ParsesOverridesAndComments` (config plumbing); real shape in decision 0004 | PASS |
| `reasoning_effort: "none"` and `stream: false` in every request | `llm.hygiene` request-shape assertion (fake server record) | PASS |
| `finish_reason: "length"` = failed attempt | `GateFunnel.TruncatedReplyIsAFailedAttempt`, `llm.truncating_20`, `ParseLlm`* | PASS |
| Text only, no image input | `onion.smoke` (no vision code in tree) | PASS |
| Keys never in logs/sync/traffic | `llm.hygiene` authorization-strip assertion (fake server record); key travels only in the Authorization header | PASS |
| Metering: usage fields (cache hit/miss, output) | `Metering.UsesOffpeakOnlyForCallsInsideOneWindow` | PASS |
| Metering: call spanning a price-window boundary billed at the higher triplet | `Metering.SpanningCallBillsTheHigherTriplet` (added W3) | PASS |
| Errors per decision 0006 (429 retry+halve; 503 halve; >5% timeouts; 400/422; 401/402 breaker after 3) | `llm.closed_port_1000`, `llm.reset_connections_1000`, `llm.timeout_1000`, `llm.auth_401`, `llm.balance_402`, `llm.client_error_400`, `CircuitBreaker.*` | PASS |
| Timeout retains the full reservation as uncertain spend | `Budget.TimeoutKeepsFullReservationAsUncertainSpend` | PASS |
| Reservations never overspend concurrently | `Budget.ConcurrentReservationsNeverOverspend` | PASS |
| A ticket that doesn't fit waits, starts after a release (G9) | `EngineTest.WaitingTicketStartsWhenTheReservationIsReleased` | PASS |
| Breaker: 20 consecutive transport failures; a successful response resets | `CircuitBreaker.*`, `llm.timeout_1000` | PASS |
| Prompt caching: fixed instructions first, node text last | `GatePrompts.FixedTextPrecedesNodeText` (added W3) | PASS |
| Stop when no reservation fits; budget refill by hand | `EngineTest.ExhaustedCampaignStaysRunningAndResumesOnBudgetRaise`, `Budget.*` | PASS |
| Spillover | M7/M8 (not yet implemented) | GAP → M8 |

## Concurrency

| DESIGN rule | Test(s) | Status |
|---|---|---|
| Goodput signal: completed output tokens/s, EWMA over two windows | `Limiter.GoodputSignalIsAnEwmaOverTwoWindows` | PASS |
| Ramp ×1.25 while goodput rises ≥5% per increase; revert + hold two windows | `Limiter.MultipliesWhileGoodputRisesFivePercent`, `Limiter.RevertsToThePreviousLimitWhenGoodputDoesNotRise` | PASS |
| Plateau probe ×1.1 after 10 windows, raw-window comparison | `Limiter.PlateauProbeAdoptsWhenCapacityGrew`, `Limiter.PlateauProbeReturnsAndRestartsTheCounter` | PASS |
| Tail guard: p95 > 2× baseline → ×0.9, hold, plateau | `Limiter.TailGuard*` (4 tests) | PASS |
| Hard backoff: 429/503, >5% timeouts, >2% connect failures → halve, hold | `Limiter.Halves*` (4 tests) | PASS |
| Floor 1 (recovers), ceiling from profile | `Limiter.NeverExceedsCeilingAndNeverFallsBelowOne`, `Limiter.RecoversFromTheFloorAfterAHardBackoff` | PASS |
| Hourly probe caps, records median+p95 baselines, restores | `Limiter.HourlyProbeCapsRestoresAndRecordsTheP95Baseline` | PASS |
| Settles within ±10% of the plateau and stays; 90% of N in 3 min | `Limiter.SettlesWithinTenPercentOfThePlateauAndStays`, `Limiter.ReachesNinetyPercentOfThePlateauWithinThreeMinutes` | PASS |
| One DNS lookup per host per TTL | `llm.hygiene` (dns_one_lookup_per_host), `ClientHygiene.ResolverMemoExpiresPerTtl` | PASS |
| New connections at most the configured rate | `llm.ramp` | NIGHTLY |
| Connections reused; HTTP/2 where offered | `llm.hygiene` (connection_reuse); negotiated version recorded in decision 0004 | PASS |
| Thread count independent of in-flight | `llm.threads` | NIGHTLY |
| Wiring against the fake server scenarios | `llm.scenarios`, `llm.capacity_shift` | NIGHTLY |
| File-descriptor ceiling | `PlatformFileLimits.RaisesMacOSSoftLimitToAtLeast4096`, `PlatformFileLimits.ReservesDescriptorsForEngineResources`; `outbound_request_ceiling` used by the client | PASS |

## Quality gate parameters

| DESIGN rule | Test(s) | Status |
|---|---|---|
| Pre-check ≤ 400 tokens, clear no → leaf without an attempt | `GatePrompts.PrecheckPromptIsAtMostFourHundredTokensApprox`, `GateFunnel.PreCheckNoMakesALeafWithoutSpendingAnAttempt` | PASS |
| 3 to 100 children | `GateChecks.CountMustBeBetweenThreeAndOneHundred` | PASS |
| No repeat of node/sibling/existing child (canonical title) | `GateChecks.ChildRepeating*` (4 tests) | PASS |
| Ancestor similarity 0.85 (parent) / 0.95 (above) | `GateChecks.AncestorSimilarityThresholds` | PASS |
| Style gates | `GateChecks.StyleGatesRejectBadTitles` | PASS |
| Decide: pass ≥ 0.7; ≤3 attempts at temperatures 0/0.3/0.5 with issues injected; floor 0.5; else NEEDS_REVIEW | `GateDecide.*` (6 tests), `GateFunnel.RetryInjectsPreviousIssuesAndCommitsBetterAttempt` | PASS |
| Nothing committed before the decision (P7) | `GateFunnel.*`, `StateWriterTest.CrashBetweenDurableWriteAndFoldLosesNothing` | PASS |
| Quality guard vs fleet median (local fixed threshold) | `QualityGuard.*` (2 tests) | PASS |
| Atomic verification (`admits_children`) | M8 (light sweep not yet implemented) | GAP → M8 |

## Cost and projections

| DESIGN rule | Test(s) | Status |
|---|---|---|
| Projection scales linearly in exact decimals | `ProjectCost.ScalesLinearlyInExactDecimals` | PASS |
| 7-day average or 20-attempt calibration; peak AND off-peak totals; log growth; spend per hour | W4 (`campaign plan`) | GAP → W4 |

## Notes

- The M6/M7 areas (sync, lanes, stealing, membership) are Agent M's and are
  out of this audit's scope.
- NIGHTLY tests skip in ci.yml (exit 77) and run in `.github/workflows/opt-nightly.yml`
  (see AUDIT.md §6); the deterministic control-law coverage above is the CI
  guarantee.
- `ParseLlm.*` (LLM response parser robustness) is listed under the provider
  section because the parser consumes the provider's wire format; the W3
  libFuzzer target `llm_response_fuzzer` adds its crash coverage.
