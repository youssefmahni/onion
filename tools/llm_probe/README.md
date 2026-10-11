# tools/llm_probe — DeepSeek concurrency probe

Finds how many parallel requests make DeepSeek fastest for one account. It
runs the same small request at several concurrency levels (one thread = one
request in flight) and reports, per level: answers per second, latency
percentiles, and errors (429 rate limits, 5xx, timeouts, network failures).

This is the probe behind the owner's real-DeepSeek measurements recorded in
`docs/agent/opt/OPTIMIZATION_GOAL.md` §4.2 and the CSVs in
`docs/agent/opt/evidence/concurrency/`. It is a measurement tool, not part of
the engine; the engine's own client follows the same hygiene rules
(shared DNS cache, reused connections, HTTP/2 preference).

## Build

```bash
cmake -S . -B build --preset <your preset>   # or a plain configure
cmake --build build --target deepseek_probe
```

The target links only libcurl and needs nothing else from the engine.

Standalone (no CMake):

```bash
g++ -O2 -std=c++17 tools/llm_probe/deepseek_concurrency.cpp -lcurl -pthread -o /tmp/deepseek_probe
```

## Run

```bash
# The key is read from the environment and never printed:
DEEPSEEK_API_KEY="$(cat ~/.config/onion/deepseek_key)" ./deepseek_probe \
    --model deepseek-flash --reasoning-none \
    --levels 12,1000 --per-level 100 --ramp 5
```

Options:

| Option | Meaning |
|---|---|
| `--levels 1,4,12,…` | Concurrency levels to test, in order (default 1,4,12,25,50,100,200) |
| `--per-level N` | Requests per level (default `max(30, 3 × level)`) |
| `--per-thread N` | Requests per thread at every level (`N × level`; min 2 for a steady rate) |
| `--ramp S` | Spread thread starts over S seconds (default 0), to avoid a burst of new connections |
| `--stop-flat P` | Stop when a level is less than P% faster than the previous one (limit reached) |
| `--max-tokens N` | Answer length per request (default 16: cheap) |
| `--model ID` | Model id (default `deepseek-chat`; check `GET /models`) |
| `--url URL` | Endpoint (default `https://api.deepseek.com/chat/completions`) |
| `--timeout S` | Per-request timeout in seconds (default 120) |
| `--reasoning-none` | Add `"reasoning_effort":"none"` (recommended: Onion sends it) |
| `--list-items N` | Ask for a numbered list of N items, so answers are long like real Onion replies (`max_tokens` defaults to `N × 15 + 50`) |
| `--max-total N` | Safety cap on total requests (default 5000) |
| `--price-in X --price-out Y` | USD per 1M tokens, to print a cost estimate |
| `--csv FILE` | Also write the results table as CSV |
| `--no-stop` | Keep going after a level shows errors |
| `--yes` | Skip the confirmation prompt |

## Reading the result

The best level is the smallest one that reaches about 90% of the top speed
with no errors. Past the peak, throughput falls while p95/p99 rise and the
median barely moves — the median alone cannot detect saturation
(finding F2 in the OPT goal).

## Hygiene built in (what W2 must also give the engine client)

- One shared DNS cache and TLS session cache for all threads (`CURLSH` with
  `CURL_LOCK_DATA_DNS` + `CURL_LOCK_DATA_SSL_SESSION`), so the hostname is
  looked up once instead of once per thread (finding F3).
- `CURLOPT_DNS_CACHE_TIMEOUT 600` ≥ the record TTL.
- One easy handle per thread, reused across requests (connection reuse).
- `--ramp` spreads the start of new threads over seconds, avoiding a burst of
  new connections.

## Safety

- Caps on total requests (`--max-total`) and per-request timeout.
- Never print the API key; it travels only in the `Authorization` header.
- Heavy repeated testing triggered a temporary block on the owner's account
  (finding F4). Space runs out; do not repeat large levels back-to-back.
