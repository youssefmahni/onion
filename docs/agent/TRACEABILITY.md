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
