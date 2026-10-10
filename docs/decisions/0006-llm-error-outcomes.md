# LLM error outcomes

## Context

Silent network outages must not leave the device retrying indefinitely, while rate
limits must not be mistaken for provider unavailability.

## Decision

| Outcome | Retry | Limiter | Breaker |
|---|---|---|---|
| OK / truncated | no | OK contributes latency | reset |
| 429 | backoff / Retry-After | halve | no count |
| 500 / 503 | backoff | 503 halves | counts toward 20 |
| Timeout | failed call | contributes to 5% threshold | counts toward 20 |
| Transport | failed call | none | counts toward 20 |
| 400 / 422 | no | none | counts toward 20 |
| 401 / 402 | no | none | opens after 3, reason auth/balance |

A timeout consumes its full reservation as spent and is marked uncertain in the
metering log.

## Source

DeepSeek's [official error-code page](https://api-docs.deepseek.com/quick_start/error_codes/), checked 2026-10-10, identifies 400 invalid format, 401 authentication failure, 402 insufficient balance, 422 invalid parameters, 429 rate limit, 500 server error, and 503 overload.
