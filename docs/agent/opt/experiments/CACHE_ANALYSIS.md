# W5 pre-V6 finding: why the prompt cache never engages (evidence)

Zero-cost analysis of the W5 baseline cassette (82 real completions through
the recording proxy, 2026-10-11). This is the input to the V6 experiment.

## 1. The parser is right; the provider reports zero hits

Every raw response carries the real field names, and the client's
`parse_response` reads exactly those keys:

- `usage` fields seen in all 82 responses: `prompt_cache_hit_tokens`,
  `prompt_cache_miss_tokens`, `completion_tokens`, `prompt_tokens`,
  `prompt_tokens_details`.
- 82 calls: `prompt_cache_hit_tokens` = **0 in every response**;
  71,928 miss tokens total. The provider itself never reported a hit, so the
  client mapping is correct and there is nothing to fix there.

## 2. Prefix lengths per prompt type (common leading text across calls)

Measured as the longest identical prefix across calls of the same type
(system message + the fixed user text; ~4 chars per token):

| Prompt type | Calls | Identical prefix (chars) | ~tokens |
|---|---|---|---|
| worker | 48 | 360 | 90 |
| trimmer | 12 | 274 | 68 |
| planner | 6 | 474 | 118 |
| critique | 6 | 389 | 97 |
| synthesizer | 6 | 626 | 156 |
| precheck | 4 | 526 | 131 |

System message: 233 chars, ~58 tokens, **identical in every call** (and
first in the messages array — nothing variable precedes it).

## 3. Anything variable before the fixed instructions

- Nothing precedes the system message (messages = [system, user]).
- Since the W3 prompt-order fix, every prompt's user text starts with its
  fixed instructions. Two honest exceptions that shorten the shared prefix:
  1. The **worker** embeds the node title inside its fixed block
     ("Every item MUST be a direct child of \"<title>\""), so workers of
     *different* nodes diverge after ~90 tokens.
  2. The **trimmer**'s keep-count is variable ("keep the best 400 → 300 →
     150"), so its shared prefix ends at "keep the best " (~68 tokens).
- The six prompt types share **only the 58-token system message**: the
  longest cross-type prefix is 58 tokens, and calls of different types are
  interleaved (precheck → planner → workers → trimmer → critique →
  synthesizer).

## 4. DeepSeek's caching rules (api-docs.deepseek.com/guides/kv_cache)

- Enabled by default; a hit requires **fully matching a persisted "cache
  prefix unit"**.
- Units persist (a) at the end of the user input and the end of the model
  output of each request, (b) when a **common prefix across multiple
  requests** is detected, (c) at fixed token intervals for long inputs.
- No documented minimum prefix length; construction "takes seconds";
  best-effort.

Reading against our data: the only prefix shared across the interleaved
types is the 58-token system message, and the per-type prefixes (68–156
tokens) are interrupted by other types between same-type calls. The observed
0-hit ratio is consistent with these units being too small/short-lived to
persist — the same finding M2/M3 already recorded.

## 5. V6 design (from the docs, not guesses)

V6 maximizes the cache-hit prefix by construction:

1. **One long shared fixed block.** Prepend a single ~1,000+ token fixed
   "knowledge-curation instruction block" to *every* prompt type (all six
   user messages start with the same text; the type-specific instructions
   follow). This makes the cross-type common prefix large enough that the
   common-prefix persistence (rule b) can engage for every call after the
   first few, regardless of interleaving.
2. **Move all per-node variability to the end** (already done for the node
   line; additionally move the worker's node title out of its fixed block so
   same-type prefixes no longer diverge per node).
3. **Measure the warm-up:** report the cache-hit ratio per call index (first
   N calls vs the rest) so the persistence/warm-up behaviour is visible in
   the cassette instead of assumed.

Cost check before V6: the longer block adds ~1k input tokens per call on the
first call of every unit, saved on every later hit; the experiment measures
the net effect (V6 in §7 of the goal).
