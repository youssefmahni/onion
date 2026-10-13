# COST_MODEL — campaign projections (OPT track, W4)

Formulas and parameters of the `campaign plan` projection (decision 0016).
Every number below is reproducible from one command (the exact invocations
are listed; the binary is `build/<preset>/data_depot`).

## Parameters (defaults; replaced by measured store data when present)

| Parameter | Default | Source |
|---|---|---|
| children kept per node | 40 | legacy synthesizer padding (will be measured, W5) |
| atomic share | 0.30 | placeholder (measured in W5) |
| attempts per node | 1.3 | retry expectation (measured in W5) |
| calls per attempt | 8 | planner + workers + trims + critique + synthesizer |
| input tokens per call | 2,200 | brief §4.3 (≈25k input per node / ≈11 calls) |
| output tokens per call | 1,700 | brief §4.3 (≈20k output per node) |
| pre-check | 1 call, 200 in / 50 out tokens | brief |
| cache-hit ratio | 0.00 | measured (baseline: 0 hits; V6 targets) |
| prices peak / off-peak per 1M | 0.30/0.006/1.20 · 0.15/0.003/0.60 | decision 0004 |
| output tokens/s | 60,000 | measured stable plateau: 58–65k at 700–800 in flight, ~77k peak at 1,000 (F1) |

**Growth rule.** `new_nodes(L+1) = expandable(L) × children_kept ×
(1 − atomic_share)`. With the defaults the per-level factor is 40 × 0.7 =
**28**.

**Where ×28 comes from.** `children_kept = 40` is the legacy-funnel estimate
(the legacy synthesizer pads to 100 children; 40 is the W5 V4 target, not
yet measured) × `(1 − atomic_share)` with `atomic_share = 0.30` (a
placeholder). Projections built from these defaults are therefore labelled
**legacy funnel (pads to 100)**. Once the W5 baseline finishes, these
parameters come from the new engine's measured settle records (children
kept, atomic share, attempts) and this file is regenerated.

**Time.** `time = projected output tokens ÷ output_tokens_per_s` — the
token rate, not answers/s, because the funnel's calls average about 1,400
output tokens (the probe's tiny answers were about 365). Money =
tokens × price / 1M (the engine's integer-decimal metering); spend per
hour = cost ÷ hours.

## Projection tables (2026-10-12, current defaults — labelled
## **legacy funnel (pads to 100)**: children_kept 40 and atomic_share 0.30
## are estimates, not measurements)

### A. P01+P06, one more level (existing corpus processed once; the new
### children are not re-expanded)

```
campaign plan t1.json --depth-to 6 --one-level --from-counts 9,17,95,818,15779,91047,208499
```

| Quantity | Value |
|---|---|
| expandable nodes | 316,264 |
| new nodes (one level of children) | 8,855,392 |
| attempts / calls | 411,144 / 3,605,411 |
| input (miss/hit) · output tokens | 7.30B / 0 · 5.61B |
| cost peak / off-peak | $8,918.64 / $4,459.32 |
| time at 60k output tokens/s | 25.96 hours |
| spend per hour peak / off-peak | $343.55 / $171.78 |

### B. The 7 empty phases, grown to today's depth (L6)

```
campaign plan t2.json --depth-to 5 --from-counts 7
```

| Quantity | Value |
|---|---|
| new nodes | 3,498,166,644 |
| attempts / calls | 162.4M / 1.42B |
| input · output tokens | 2.88T · 2.22T |
| cost peak / off-peak | $3,523,153.55 / $1,761,576.77 |
| time at 60k output tokens/s | 10,255.04 hours (~427 days) |
| spend per hour peak / off-peak | $343.55 / $171.78 |

### C. The full tree to L9 (everything expanded)

```
campaign plan t3.json --depth-to 9 --from-counts 9,17,95,818,15779,91047,208499
```

| Quantity | Value |
|---|---|
| new nodes | 3.01 × 10^15 |
| attempts / calls | 1.40 × 10^14 / 1.23 × 10^15 |
| cost peak / off-peak | $3.03T / $1.52T |
| time at 60k output tokens/s | 8.82 × 10^9 hours |

**Conclusion at the current defaults:** the full tree to L9 is infeasible by
about six orders of magnitude. The lever is the per-level growth factor 28:
the brief's own arithmetic (§4.3) says ×3 vs ×15 per level is the difference
between about two months and about two years. W5's variants (children 100 →
40 → fewer, higher atomic share, the light funnel) change exactly this
factor, and the measured parameters (attempts, tokens per node, cache-hit
ratio) will replace the labelled defaults here — this table is re-generated
from the same commands after W5.

## Known limitations

- The store-based path (`campaign plan` without `--from-counts`) is
  truncated by the dev import, which stops at the first duplicate path (the
  corpus's 131 known duplicates at L4, DESIGN P3). The genesis import (M4)
  resolves duplicates properly; until then use `--from-counts` with the
  corpus counts.
- The cache-hit ratio is 0 in the baseline (see `CACHE_ANALYSIS.md`); the
  input columns above are all misses. V6 changes this.
- Time assumes one device at the stable plateau; a fleet divides the hours
  by the effective parallel throughput.
