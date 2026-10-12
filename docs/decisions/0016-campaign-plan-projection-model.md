# 0016 — Campaign plan projection model

Status: **Accepted** (implements DESIGN.md › Campaigns "Cost projection first"
and OPT goal O4; the formulas are an implementation detail of that rule).

## Context

`campaign start` currently projects `loop_budget × average attempt cost`,
which says nothing about the tree's growth, the time it takes, or the spend
per hour. The OPT goal O4 requires: expansions per level, input/output tokens
with the cache split, peak and off-peak cost, time at the current request
limit, and spend per hour — computed from measured data before the campaign
starts.

## Decision

A pure function `sched::plan_campaign` projects a campaign from:

- the starting expandable-node count per level (from the store for the
  campaign's scope, or from the corpus counts);
- the target depth (levels to expand);
- parameters that come from the store's measured records when present and
  fall back to clearly labelled defaults otherwise: children kept per node,
  atomic share, average attempts per node, calls per attempt, input/output
  tokens per call, the cache-hit ratio, the peak/off-peak price triplets
  (decision 0004), and the stable answers-per-second rate (the measured
  700–800 in-flight plateau, finding F1).

The growth rule: `new_nodes(L+1) = expandable(L) × children_kept ×
(1 − atomic_share)`. Costs use the same integer-decimal metering as the
engine. Time = total calls ÷ answers_per_s. Spend per hour = tokens per hour
× price at the current request limit. The projection prints both the peak
and off-peak totals.

`campaign start` shows this projection before the confirm prompt, and a new
`campaign plan <file>` prints it without starting a campaign.

## Consequences

- O4 becomes reproducible from one command and testable: the arithmetic has
  unit tests (growth, cache split, peak/off-peak totals, time, atomic
  stop), and `docs/agent/opt/COST_MODEL.md` holds the formulas and the
  corpus projection tables, refreshed after W5's measurements.
- The old `loop_budget × average cost` line is replaced; the store fallback
  keeps it working on an empty store.

## Update 2026-10-12 — time from the output-token rate

The owner's correction: time is computed from the projected output tokens
divided by the measured stable **output-token rate** — about 58–65k tokens/s
at 700–800 in flight, about 77k peak at 1,000 (finding F1) — not from
answers/s. The probe's answers were about 365 tokens, while the funnel's
calls average about 1,400, so an answers/s rate understates the funnel's
time by roughly the same factor. `PlanParams.answers_per_s` is replaced by
`output_tokens_per_s` (default 60,000); the growth and cost formulas are
unchanged.
