# 0004 — DeepSeek model and price configuration

## Context

M2 needs a model identifier and per-million-token prices for metering. The
official DeepSeek pricing page lists separate peak and off-peak rates, so one
hard-coded price would silently become wrong during part of each week.

## Options

1. Hard-code one current rate in the client.
2. Use the documented Flash model name and require the active rates in
   `node.conf`.

## Choice

Use `deepseek-flash` as the M2 model identifier. `node.conf` must explicitly
provide `llm.price_in`, `llm.price_cache`, and `llm.price_out`; the client has
no implicit monetary defaults.

On 2026-10-10, the official source was
https://api-docs.deepseek.com/quick_start/pricing/. It listed, per million
tokens: peak input cache miss $0.30, cache hit $0.006, output $1.20; off-peak
input cache miss $0.15, cache hit $0.003, output $0.60. Operators select the
applicable rate in `node.conf` and update it when DeepSeek changes pricing.

## Consequences

Metering is reproducible from the recorded configuration and never guesses a
price. Tests use explicit fixture prices only. A live `/models` request was
not made because M0 was already tagged and its allowed call step was skipped.
