# COST_MODEL — campaign projections (OPT track, W4/W5)

Formulas and parameters of the `campaign plan` projection (decision 0016).
Every number below is reproducible from one command (the exact invocations
are listed; the binary is `build/<preset>/data_depot`).

## Parameters (W5 baseline measured; `measure_params` fills them from the store)

| Parameter | Value | Source |
|---|---|---|
| children kept per node | 54.25 | W5 baseline measured, n=4 non-leaf settles: kept {100, 100, 10, 7} |
| atomic share | 0.49 | measured VERIFIED share, DR 0021 (166/340 on the W5 sample) |
| attempts per node | 1.75 | W5 baseline measured: 21 attempts / 12 settles (attempts table) |
| calls per attempt | 8 | engine |
| input tokens per call | ~570 | measured: 95,784 in / 21 attempts / 8 calls |
| output tokens per call | ~1,128 | measured: 189,440 out / 21 attempts / 8 calls |
| pre-check | 1 call, 200 in / 50 out tokens | brief |
| cache-hit ratio | 0.00 | measured (baseline: 0 hits; V6 targets) |
| prices peak / off-peak per 1M | 0.30/0.006/1.20 · 0.15/0.003/0.60 | decision 0004 |
| output tokens/s | 60,000 | measured stable plateau: 58–65k at 700–800 in flight, ~77k peak at 1,000 (F1) |

**Growth rule.** `new_nodes(L+1) = expandable(L) × children_kept ×
(1 − atomic_share)`. With the measured parameters the per-level factor is
54.25 × 1.0 = **54.25** — higher than the legacy estimate (40 × 0.7 = 28)
because the measured children kept (54.25) exceeds the legacy 40 and no
child measured atomic (0 vs 0.30).

**Where the factor comes from.** `children_kept = 54.25` is the mean of the
four W5 settles that kept children, `{100, 10, 7, 100}` (`settles.kept`,
`~/w5-base/data/onion.db`): the two mid-branch paths kept exactly 100 —
the synthesizer pads to 100 (F2, `W5_BASELINE.md`) — and the two terminal
ones kept 10 and 7. `atomic_share = 0` because none of the 217 kept
children was atomic. `attempts_avg = 21/12` from the attempts table
(review-accepted settle rows record attempts 0, so `measure_params`
prefers the attempts table — PR #31). Caveat: n=4 non-leaf paths, all
settled through the review queue (no path passed the 0.7 gate); the
sample is small and review-accepted, so these are baseline measurements,
not recommendations. The old legacy-funnel defaults (40 / 0.30 / 1.3,
labelled **legacy funnel (pads to 100)**) live in git history.

**Time.** `time = projected output tokens ÷ output_tokens_per_s` — the
token rate, not answers/s, because the funnel's calls average about 1,400
output tokens (the probe's tiny answers were about 365). Money =
tokens × price / 1M (the engine's integer-decimal metering); spend per
hour = cost ÷ hours.

## Projection tables (2026-10-13, decision-0017 configuration: the
## target 8-20 midpoint 14 with the measured verified atomic share 0.49 —
## growth 7.14; synthetic params in `~/w54/combined`)

### A. P01+P06, one more level (existing corpus processed once; the new
### children are not re-expanded)

```
campaign plan campaign.json --depth-to 6 --one-level --from-counts 9,17,95,818,15779,91047,208499 --data-dir ~/w5-base/data
```

| Quantity | Value |
|---|---|
| expandable nodes | 316,264 |
| new nodes (one level of children) | 2,213,848 |
| attempts / calls | 474,399 / 4,111,432 |
| input (miss/hit) · output tokens | 1.02B / 0 · 1.59B |
| cost peak / off-peak | $3,356.11 / $1,678.06 |
| time at 60k output tokens/s | 11.15 hours |
| spend per hour peak / off-peak | $301.02 / $150.51 |

### B. The 7 empty phases, grown to today's depth (L6)

```
campaign plan campaign.json --depth-to 5 --from-counts 7 --data-dir ~/w5-base/data
```

| Quantity | Value |
|---|---|
| new nodes (`--grow-levels 3` on all 316,264) | 126,189,336 |
| attempts / calls | 27.04M / 234.35M |
| input · output tokens | 88.59B · 137.27B |
| cost peak / off-peak | $191,298.53 / $95,649.26 |
| time at 60k output tokens/s | 635.50 hours (~26 days) |
| spend per hour peak / off-peak | $301.02 / $150.51 |

Note the scope change: the earlier B table grew the seven empty phases
from their 7 roots; this one is the whole existing tree grown three
waves (the owner's ~g²-ratio semantics; see DR 0017).

### C. The full tree to L9 (everything expanded)

```
campaign plan campaign.json --depth-to 9 --from-counts 9,17,95,818,15779,91047,208499 --data-dir ~/w5-base/data
```

| Quantity | Value |
|---|---|
| new nodes | ≥2.10 × 10^18 (saturated at int64) |
| attempts / calls | ≥6.78 × 10^16 / ≥5.81 × 10^17 |
| cost peak / off-peak | ≥$26.91T / ≥$13.45T |
| time at 60k output tokens/s | ≥8.64 × 10^10 hours |

The depth-9 projection saturates the int64 arithmetic (the plan CLI clamps
at INT64_MAX and prints the saturation note — PR #31); the true values are
larger. The magnitude conclusion is unchanged and stronger than the legacy
defaults: the full tree to L9 is infeasible by six-plus orders of
magnitude, and the measured growth factor (54.25) is *higher* than the
legacy 28, which makes the W5 variant work (children 100 → fewer, higher
atomic share, the light funnel) the decisive lever, not a refinement.

## Known limitations

- The store-based path (`campaign plan` without `--from-counts`) reads the
  data dir's nodes; for the full corpus pass `--from-counts` with the
  corpus counts. (The dev import no longer truncates at duplicates —
  PR #29 — but the baseline data dir is a 12-path sample store.)
- The cache-hit ratio is 0 in the baseline (see `CACHE_ANALYSIS.md`); the
  input columns above are all misses. V6 changes this.
- Time assumes one device at the stable plateau; a fleet divides the hours
  by the effective parallel throughput.
- The measured children kept is review-accepted (no gate pass), n=4;
  `W5_BASELINE.md` F1–F6 give the full evidence.
