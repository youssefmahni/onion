# OPT track — final report

Scope: OPTIMIZATION_GOAL.md §5–§7. Worktree `~/Desktop/onion-opt`, all
work on `opt/*` branches, merged to `main` as PRs #13–#34 (Agent M's
area untouched throughout).

## Workstreams

- **W1 setup/audit** — PRs #13–#15: the brief, the board, the probe tool
  (`tools/llm_probe`), the concurrency evidence, `AUDIT.md`. Found the
  existing funnel state, the missing client items, and the traceability
  gaps that set up W2/W3.
- **W2 client + limiter** — PRs #16–#20, #24: limiter v2 (goodput EWMA,
  ×1.25 growth, plateau probe, p95 tail guard — decisions 0011), client
  hygiene (CURLSH caches, HTTP/2 with stream caps, connection ramp,
  resolver hook), fake-server scenarios, profile rules v2 (0012,
  Proposed), the C5 smoke (2 billed calls; HTTP/2 confirmed). The
  macOS-slowdown question was answered with local evidence: harness-side
  overhead (2.3 ms client cost), not the client.
- **W3 hardening** — PRs #23–#24: the traceability audit, flake fixes
  (state crash-test flake), 5 libFuzzer targets + the nightly fuzz job,
  C4 kill-point crash-consistency tests, fold idempotency (settle
  score-first, attempts/critiques `INSERT OR IGNORE` on `(origin, hlc)`),
  prompt-cache prefix reorder, the HTTP pin (0015).
- **W4 projection** — PRs #28, #30, #32: `campaign plan` with measured
  parameters (`measure_params`), the 0016 model, `COST_MODEL.md`
  (regenerated twice: defaults, then the owner's token-rate time
  formula, then W5's measured values), int64 saturation for deep
  projections, the dev-import duplicate-path fix (PR #29: keeps the
  first row, reports 131 skips, no more truncation).
- **W5 baseline + variants** — PRs #31–#34: the 12-path baseline
  (12/12 settled; F1–F6 in `W5_BASELINE.md`), the variant knobs behind
  campaign-spec fields, V1–V7 one-factor-at-a-time on the real API with
  the committed drivers, blind pairwise judging, `FUNNEL_OPT.md`, and
  the proposed decision 0017 (funnel defaults v2).
- **W6** — the two-network concurrency OA is written (OA-3, owner-run).
- **W7** — the M6 convergence review was done early by the owner's
  order (PR #21); nothing in Agent M's area changed.

## Key findings

1. **The funnel is the cost.** The measured growth factor is
   children_kept × (1 − atomic_share): 54.25 at baseline (kept
   {100,10,7,100}; the synthesizer pads to 100), 10–25 under the
   variants. The critique scores average ~0.1–0.3 against a 0.7 gate:
   no path ever passed the gate in the engine loop; the review rate was
   33% at baseline.
2. **W5 variants move both dials.** V3 (single trim to 150): −53% in /
   −40% out tokens, 3/3 judged wins. V2 (3–5 aspects): 4/4 wins. V6
   (shared cache prefix): nonzero measured cache hit, best pass rate.
   V7 (light funnel from L6): half the tokens, 4.2× kept children on L6
   nodes. V4 (capped synthesizer) does not pay: excluded. Full tables
   in `FUNNEL_OPT.md`; the combined recommendation is DR 0017, behind
   configuration, awaiting the owner.
3. **Cache.** Zero prompt-cache hits at baseline (no shared prefix);
   V6's prefix measured the first hits (128 cached tokens). The
   prompt-cache design (fixed text first) is in place; V6 is the knob.
4. **Budgets.** Real API: ~1,403 / 1,500 calls, ~$1.19 / $5.00. W5
   exceeded its own 1,000-call allocation by ~266 (scope correction +
   V7 pair) — disclosed in COORDINATION.md and PROGRESS.md; W5.4
   (generalization) deferred pending the DR 0017 decision, and no
   further spend happens without the owner's approval.

## Definition of done vs the brief (§10)

- O1/O2 (throughput control): done — limiter v2 + hygiene, tests on the
  fake server, decisions 0011/0012.
- O3 (funnel cost/quality): done through the proposal stage — baseline,
  variants, judges, FUNNEL_OPT.md, DR 0017 proposed; the final
  generalization run and the default change await the owner.
- O4 (projection): done — plan_campaign, measured params, saturated
  arithmetic, COST_MODEL regenerated three times.
- F6 (fleet speed-up): prepared as OA-3 (owner-run).
- C1–C5: done with evidence.
- Integrity rules: obeyed — Agent M's files untouched, secrets never
  committed (pre-commit scans), tests never weakened, append-only
  ledgers kept, two-axis reviews before every push.

## Open items for the owner

1. **DR 0017** (funnel defaults v2): accept / reject / subset. Until
   then the baseline defaults stay effective.
2. **W5.4 generalization** (≈135 calls): only after the DR 0017 call.
3. **OA-3** (W6 two-network probe): 10 minutes of your time, two
   machines, no repo changes.
