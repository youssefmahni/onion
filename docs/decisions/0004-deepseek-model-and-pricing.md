# 0004 — DeepSeek model and price configuration

## Context

M2 needs a model identifier and per-million-token prices for metering. The
official DeepSeek pricing page lists separate peak and off-peak rates, so one
hard-coded price would silently become wrong during part of each week.

## Options

1. Hard-code one current rate in the client.
2. Configure one active rate and change it manually at every window boundary.
3. Configure peak and off-peak rates plus their UTC window.

## Choice

Use `deepseek-flash` as the M2 model identifier. `node.conf` explicitly
provides `llm.price_peak_in`, `llm.price_peak_cache`, `llm.price_peak_out`,
`llm.price_offpeak_in`, `llm.price_offpeak_cache`, `llm.price_offpeak_out`,
and `llm.offpeak_utc` (weekly UTC intervals). The client has no implicit
monetary defaults.

On 2026-10-10, the official source was
https://api-docs.deepseek.com/quick_start/pricing/. It listed, per million
tokens: peak input cache miss $0.30, cache hit $0.006, output $1.20; off-peak
input cache miss $0.15, cache hit $0.003, output $0.60. It states: “Peak
hours are 01:00 - 04:00 and 06:00 - 10:00 UTC, Monday through Friday.”

The page does not state whether the price for a request crossing a window
boundary is selected at request start, request completion, or otherwise. A
call whose start and end timestamps are both inside one configured window is
metered at that window's triplet. Until DeepSeek documents the crossing rule,
a call spanning a boundary is metered at the higher triplet. Reservations
always use the maximum of the two price triplets, preventing an overspend.
Cost projections display both peak and off-peak totals.

## Consequences

Metering is reproducible from the recorded configuration and never guesses a
price. Tests use explicit fixture prices only. A live `/models` request was
not made because M0 was already tagged and its allowed call step was skipped.

## Update 2026-10-10 — live validation findings (M2 real-call validation, 5 completions + free GET /models)

Performed by the agent on 2026-10-10 with a real key (env `DEEPSEEK_API_KEY`,
never printed). Findings:

- `GET /models` (free) over HTTPS negotiates **HTTP/2** and lists
  `deepseek-flash` (DeepSeek-V4.1-Flash, context 1,048,576, max output
  393,216) and `deepseek-v4-pro`. The decision's model id is confirmed.
- The request shape with `reasoning_effort: "none"` is **accepted** (200 OK),
  although the models list advertises supported effort levels `low`, `high`
  and `max` with default `high`. No rejection observed; keep `"none"` until a
  rejection or behaviour change is observed.
- The usage field names the client parses are the real ones:
  `prompt_cache_hit_tokens`, `prompt_cache_miss_tokens`,
  `completion_tokens`.
- Repeating the same short prefix (17 input tokens) produced **no cache-hit
  tokens** (`prompt_cache_hit_tokens=0` on the repeat). DeepSeek's automatic
  prompt cache did not engage for this short prefix. The client reports hits
  whenever the provider reports them; M3's real campaign (long fixed
  instruction text first) is where hits are expected to appear.
- `max_tokens: 1` returned `finish_reason: "length"`, classified by the
  client as Truncated (failed attempt).
- A deliberately invalid key returned **401** and opened the breaker with
  reason `authentication` after three consecutive responses, as specified.

Cost of the four billed completions: 68 cache-miss input + 4 output tokens,
all in off-peak hours (Saturday) → 68 × $0.15/M + 4 × $0.60/M = $0.0000126.
The 401 call is not billed.
