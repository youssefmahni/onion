# 0011 — Limiter v2: goodput control and client hygiene

Status: **Accepted** (the owner asked for this in OPTIMIZATION_GOAL.md §6 W2;
it implements findings F1–F3 of the real-DeepSeek measurements).

## Context

The v1 limiter (decision 0005) grows the request limit by +4 when the median
latency is at most 1.5× its low-load baseline. The owner's real measurements
(OPTIMIZATION_GOAL.md §4.2) show that past DeepSeek's peak (about 1,000 in
flight) the **median barely moves** while throughput falls 30% and p99 nearly
triples. A median-watching limiter therefore cannot detect saturation (F2),
and additive growth is too slow to converge in minutes. F3 shows the client
itself must not burst DNS lookups or connections.

## Decision

The limit is evaluated once per window (default 30 s, injectable for tests).

**Signals.**

- A window's *goodput* = completed output tokens of successful requests in
  that window ÷ window length. Failed requests do not count.
- The *goodput signal* G = an EWMA (α = 0.5) over consecutive successful
  windows. A window is successful when it has at least one completed token
  and no pressure event. Pressure events do not update G.
- A *pressure event* is: any 429 or 503; timeouts above 5% of calls;
  transport failures (connection failures, including failed DNS resolution)
  above 2% of calls.

**Baselines.** The first successful window at the starting limit anchors the
baseline median and p95. The hourly probe of decision 0005 is kept and now
records the p95 as well; its cap, restore and halve-on-pressure rules are
unchanged.

**Rules, in priority order (the first that applies in a window wins).**

1. **Hourly baseline probe** (decision 0005, extended to record p95).
2. **Hard backoff** on a pressure event: limit = max(1, ⌊limit / 2⌋), hold one
   window, and reset the ramp state. Unchanged from v1 except that transport
   failures above 2% are now a pressure event.
3. **Tail guard.** If a baseline p95 exists and the window's p95 exceeds twice
   it, limit = max(1, ⌊limit × 0.9⌋), hold one window, and enter the plateau
   state: the step-back plays the role of a revert, so the ramp cannot blindly
   re-increase into the danger zone, and the plateau probe keeps tracking
   capacity recovery. While the p95 stays above the threshold the guard keeps
   stepping down, one ×0.9 per non-hold window.
4. **Plateau probe.** After 10 consecutive plateau windows, one probe window
   at min(ceiling, ⌊limit × 1.1⌋): if G rose at least 5% over the plateau's
   entry signal, adopt the probed limit and resume ramping; otherwise return
   to the plateau limit and restart the counter.
5. **Ramp.** The first successful window after the starting limit or a backoff
   performs an unassessed increase. After any increase, the next successful
   window assesses it: if G ≥ 1.05 × G at the moment of the increase, increase
   again to min(ceiling, ⌊limit × 1.25⌋). An increase that does not change the
   limit (ceiling reached) ends ramping: hold at the ceiling, no revert.
6. **Revert.** If the assessment fails, return to the pre-increase limit, hold
   two windows, then start counting plateau windows (the plateau's entry
   signal is G at the revert).

**Holds.** A hold counts down one per non-idle window and blocks all
proactive changes; backoff and the tail guard may still fire during a hold.
Idle windows (no calls) are no-ops.

The limit never exceeds the ceiling and never falls below 1. The median
baseline of v1 is no longer part of the control law (it is still recorded for
the trace); the 1.5× median rule and the additive +4 rule are removed.

**Client hygiene (same milestone).**

- One curl multi handle per process (unchanged).
- A `CURLSH` share handle with `CURL_LOCK_DATA_DNS` and
  `CURL_LOCK_DATA_SSL_SESSION` is attached to every easy handle, so the
  hostname is resolved at most once per process per TTL and TLS sessions are
  reused across handles. `CURLOPT_DNS_CACHE_TIMEOUT` = `dns_cache_ttl_s`
  (default 600, at least the record TTL).
- HTTP/2 preference with multiplexing is kept; `CURLMOPT_MAX_CONCURRENT_STREAMS`
  (default 100), `CURLMOPT_MAX_HOST_CONNECTIONS` (default 6) and
  `CURLMOPT_MAX_TOTAL_CONNECTIONS` (default 12) cap the connection topology.
- **Connection ramp.** A token bucket with rate `new_connections_per_s`
  (default 10, 0 = disabled) and burst equal to the rate. Each handle added to
  the multi handle consumes one token; when a handle finishes having opened
  `k` connections it refunds `1 − k` tokens (a reuse refunds one; a redirect
  pays for the extra). When the bucket is empty, new handles wait. The
  new-connection rate is therefore at most `new_connections_per_s` plus one
  window of burst.
- **Test-only DNS hook.** `Client::Options::dns_resolver` is an optional
  callback returning the resolved address for `(host, port)`; when set, the
  client memoizes resolutions per host for `dns_cache_ttl_s` and pins every
  handle with `CURLOPT_RESOLVE`. The hook call count equals the number of
  lookups, which lets tests assert "one lookup per host" and inject
  `dns_fail_burst`. In production the hook is absent and the shared curl DNS
  cache provides the same property.

## Consequences

- O1 and O2 of the OPT goal become testable with simulated time and a fake
  server: goodput-based growth, plateau detection and revert, tail-guard
  descent, one-window backoff, one DNS lookup per host, a capped
  new-connection rate, and a thread count independent of the in-flight count.
- The v1 limiter tests and the `limiter-trace` CLI output are updated to the
  v2 rules; DESIGN.md › Concurrency is rewritten to match.
