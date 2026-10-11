# Onion Engine — Track OPT: Correctness and Optimization Brief

> **For Youssef, before handing this over**
> 1. Check the values in §0.
> 2. Send the short coordination note at the end of this file (§13) to the agent working on M6, so it knows a second agent exists.
> 3. Start the new agent with the kickoff message in §12.

---

## 0. Owner settings

| Setting | Value | Meaning |
|---|---|---|
| `REAL_CALL_CAP` | `1500` | Maximum real DeepSeek completion calls for this track (`GET /models` is free and does not count) |
| `REAL_USD_CAP` | `5.00` | Maximum real spend for this track, in USD |
| `KEY_SOURCE` | `DEEPSEEK_API_KEY`, else the file `~/.config/onion/deepseek_key` | Use the key without ever printing it (§9.3) |
| `LIVE_TREE` | `~/Desktop/PlanningProcess` | The live corpus. **Read-only, and read only once** to make your copy |
| `DATA_COPY` | `~/onion-opt-data` | Your own copy of the corpus; the only data you may write |
| `WORKTREE` | `~/Desktop/onion-opt` | Your separate git worktree. Never work in `~/Desktop/onion` |
| `MERGE_POLICY` | `agent-merges-small-prs` | You merge your own small PRs into `main` when green and rebased (§2.4) |

---

## 1. Mission

You are the **second engineer** on the Onion Knowledge Engine rebuild. Another agent ("Agent M") is building milestone **M6 (sync between machines)** at the same time, in the same repository.

Your track ("OPT") makes the engine **correct** and **optimal** in the areas Agent M is not touching, and turns real measurements into design decisions. You do not build new milestones. You harden, measure, tune and prove what already exists.

You stop only when:
- the definition of done in §10 is met;
- a hard stop occurs (§9.5); or
- everything remaining is blocked.

You record anything that needs the owner in `docs/agent/opt/OWNER_ACTIONS.md`, and keep working.

---

## 2. Coordination with Agent M (read first, follow strictly)

### 2.1 Separate working copy
- Never edit, build or run git commands in `~/Desktop/onion`; that is Agent M's working tree.
- Create your own worktree once, from the latest `main`:
  ```bash
  cd ~/Desktop/onion && git fetch origin
  git worktree add ~/Desktop/onion-opt -b opt/setup origin/main
  ```
- Work only in `~/Desktop/onion-opt`. Use your own build directories there.

### 2.2 Branches
- Every branch you create is named `opt/<topic>`, starts from the latest `origin/main`, and is short-lived.
- Never check out, commit to, push to, rebase or merge Agent M's branches (`m6-*`, `m7-*`, …).
- Never commit directly to `main`.

### 2.3 File ownership

| Area | Owner | Your rule |
|---|---|---|
| `src/sync/`, `src/swim/`, the lane and stealing parts of `src/sched/`, `tests/fleet/`, `tools/chaos/` | **Agent M** | Do not edit. Read-only review is allowed (§6, W7) |
| `docs/agent/PROGRESS.md`, `docs/agent/WORKLOG.md`, `docs/agent/gates/M6*.md`, `.github/workflows/ci.yml` | **Agent M** | Do not edit |
| `src/llm/`, `tools/llm_probe/`, `tests/fuzz/`, `docs/agent/opt/`, `.github/workflows/opt-*.yml` | **You** | Yours |
| `src/gate/`, `src/store/`, `src/log/`, `src/state/`, `src/profile/`, `src/platform/`, the campaign and budget parts of `src/sched/`, `src/cli/`, `src/api/` | Shared | Small, focused PRs only. Before touching a file, check that no open `m6-*` PR changes it (`gh pr list`, `gh pr diff`). If one does, wait or write a proposal instead (§2.5) |
| `docs/DESIGN.md`, `docs/decisions/`, `CMakeLists.txt` | Shared | Edit only the sections you own (LLM provider, Concurrency, Quality gate parameters, Cost). One decision-record number at a time: take the next free number on `origin/main` right before committing, and rebase if it gets taken |

### 2.4 Merging
- A PR merges only if all of these hold:
  - CI is green on the PR head;
  - it is rebased onto the latest `origin/main` with no conflicts;
  - it touches no Agent M file;
  - it is under about 600 changed lines, or split.
- Merge with `gh pr merge --merge`. Never force-push `main` or any branch someone else uses.
- After each of your merges, append one line to `docs/agent/opt/COORDINATION.md`: what changed, which files, whether `m6-sync` needs to merge `main` to stay current.

### 2.5 Communication
- `docs/agent/opt/COORDINATION.md` is the shared notice board. Read it at the start of every session; append only, never rewrite.
- If you find a bug in Agent M's area, do not fix it. Write a precise note in `COORDINATION.md` (symptom, evidence, suspected cause, suggested fix) and an owner action, so the owner can pass it on.

### 2.6 CI minutes
Both agents push to the same repository. Push only coherent units of work. Put any long-running jobs (fuzzing, stress, soak) in your own scheduled workflow, `.github/workflows/opt-nightly.yml`, never in `ci.yml`.

---

## 3. Sources of truth (in order)

1. The owner's messages.
2. `docs/DESIGN.md` and `docs/decisions/` on `origin/main`.
3. `docs/REBUILD_PLAN.md`.
4. `docs/agent/AUTONOMOUS_GOAL.md`: its integrity rules (§7), anti-loop rules (§8.3), evidence standard (§9.3), secret and data rules (§6.2, §6.4) apply to you unchanged.
5. This file: your scope, targets and plan.

If this file disagrees with `DESIGN.md` on a constant or a format, `DESIGN.md` wins. Propose the change through a decision record instead (§9.4).

---

## 4. What is already known (evidence to build on)

### 4.1 The tree (owner's live corpus, P01 and P06 only; the other 7 phases are empty)

| Level | Nodes |
|---|---|
| L0 | 9 |
| L1 | 17 |
| L2 | 95 |
| L3 | 818 |
| L4 (TDs) | 15,779 |
| L5 | 91,047 |
| L6 | 208,499 |
| L7–L9 | 0 |
| **Total** | **316,264** |

Only 12 nodes have been funneled (settled) and only 30 flagged atomic: almost everything came from the import. Other copies under `~/Desktop/PlanningProcess*` disagree; the live one is `~/Desktop/PlanningProcess`.

### 4.2 Real DeepSeek concurrency (owner's tests: one API key, model `deepseek-flash`, `reasoning_effort: none`)

Real-size answers (about 365 output tokens). Rows from 600 up used a shared DNS cache and a ramp of new connections:

| In flight | Answers/s | p50 | p95 | p99 | Errors |
|---|---|---|---|---|---|
| 100 | 32.8 | 2.72 s | 3.2 s | 3.3 s | 0 |
| 200 | 58.1 | 2.79 s | 3.5 s | 3.7 s | 0 |
| 400 | 98–112 | 2.7–2.9 s | 4.2–4.3 s | 5.2–5.9 s | 0 |
| 600 | 141 | 2.81 s | 3.2 s | 3.3 s | 0 |
| 700 | 160 | 2.84 s | 3.4 s | 4.0 s | 0 |
| 800 | 177 | 2.88 s | 3.5 s | 4.1 s | 0 |
| **1,000** | **204–207** | 2.65 s | 3.1 s | 3.2–3.3 s | 2 network |
| 1,400 | 144–146 (−30%) | 2.82–2.84 s | 4.8–5.0 s | 8.6–9.1 s | 0 |

Two runs of 1,000 and 1,400 agree within 2%. Run-to-run variation at the same level is up to about 30%.

**Findings that must drive the design:**
- **F1. The peak is about 1,000 in flight** (about 205 answers/s, about 77,000 output tokens/s). Stable operation is 700–800.
- **F2. Past the peak, median latency barely moves** (2.65 → 2.84 s) while throughput falls by 30% and p99 nearly triples. A limiter that watches the median cannot detect saturation; answers per window and p95/p99 can.
- **F3. The client can be the bottleneck.** Without a shared DNS cache, hundreds of simultaneous lookups failed ("Could not resolve hostname") and new connections failed or timed out at 600–700. A shared DNS cache, shared TLS sessions and a ramp removed every error.
- **F4. Heavy repeated testing caused a temporary block** (mass connection failures), which recovered within the hour.
- **F5. Budget is the binding constraint:** 1,000 in flight is about 278 million output tokens per hour.
- **F6. Untested:** whether the limit is per account or per machine.

**Probe tool and raw data:**
- The probe tool is `~/Desktop/onion/docs/deepseek_concurrency_test.cpp`, with the CSVs `rerun.csv`, `rerun2.csv` and `limit.csv` in the same folder. They are untracked in Agent M's tree: copy them (read-only) into your worktree in W1.
- That folder also holds an older binary, `ds_test`. Don't use it; rebuild from the source.

### 4.3 Token cost per node (an estimate, not yet measured)

One full-funnel attempt is about 11 calls: about 20,000 input + 15,000 output tokens. With retries and the pre-check, about 25,000 input + 20,000 output tokens per node. The real test produced about 9 output tokens per list item. The legacy synthesizer pads to 100 children.

Cost and time to fill the tree depend mostly on **children kept per node × the non-atomic share** (×3 vs ×15 per level is the difference between about two months and about two years for the full tree). These numbers must be replaced by measurements (W4, W5).

### 4.4 Open correctness concern

Agent M's multi-process convergence test was failing intermittently (about 50%), with the failure mode "campaign never finished everywhere". That is Agent M's to fix; you may only review it (W7).

---

## 5. Definitions: what "correct" and "optimal" mean here

**Correct**, with every item proven by evidence:
- **C1.** Every rule in `DESIGN.md` sections outside Agent M's area maps to at least one passing test (`docs/agent/opt/TRACEABILITY_AUDIT.md`).
- **C2.** No flaky test in the new tree outside Agent M's area: every such test passes 50 consecutive repeats under parallel load, and under ThreadSanitizer for concurrent code.
- **C3.** Parsers survive fuzzing with no unresolved crash:
  - LLM response;
  - canonical JSON;
  - batch/record;
  - CSV/TSV import;
  - HTTP request parsing in the local API.
- **C4.** Crash consistency of the single-node engine: a process killed at each step of the commit order (decision 0000) loses no committed work, never commits children before the gate decision, and resumes at the right attempt number with spend preserved.
- **C5.** Real-API smoke checks pass for the request shape and the usage-field mapping.

**Optimal**, with measurable targets:
- **O1, throughput control.** Against the fake server with a plateau at N in flight and a flat median:
  - the limiter settles within ±10% of N and stays there;
  - from the starting value it reaches 90% of N in under 3 minutes of (simulated) time;
  - it backs off within one window on rate limits, timeouts or connection failures;
  - it never exceeds the ceiling.
- **O2, client efficiency.**
  - DNS lookups: 1 per host per process per TTL.
  - New connections: at most a configured rate per second.
  - Connections: reused; HTTP/2 multiplexing used where the server offers it.
  - Thread count: independent of the in-flight count.
- **O3, cost.** Measured tokens per accepted node are reduced by at least 30% versus the measured baseline, with no statistically significant quality loss (§7). Or you document why that target is unreachable. Parameter changes go to the owner as Proposed decision records.
- **O4, transparency.** The cost and time for any campaign are projected from measured data before it starts, including spend per hour at the current request limit and per-level growth.

---

## 6. Workstreams (in this order)

### W1 · Setup and audit (first session)
1. Create the worktree (§2.1). Read `COORDINATION.md` (create it if missing, with a header explaining its purpose).
2. Copy the corpus once: `rsync -a "$LIVE_TREE"/{"Onion - level_"*.csv,funnel_state.tsv,funnel_critique.tsv,atomic_items.tsv} "$DATA_COPY"/`. Verify the counts match §4.1. Never write to `LIVE_TREE`.
3. Find out which milestones are merged (`git log --oneline origin/main`, the tags, `docs/agent/gates/`). Note what exists of M3–M5 (gate, campaigns, store, profiles).
4. Check whether these items from the owner's earlier instructions are implemented (search the decision records and the code); for each, record present / partial / missing:
   - the goodput-based limiter;
   - multiplicative ramp-up;
   - p95 hold;
   - a shared DNS cache and connection pool;
   - a ramp of new connections;
   - HTTP/2 multiplexing;
   - profile ceilings v2;
   - spend per hour in the projection.
5. Bring the probe tool into the repo as `tools/llm_probe/` (with a CMake target and a README), plus the CSV evidence under `docs/agent/opt/evidence/concurrency/`. Include a short summary of §4.2.
6. Write `docs/agent/opt/AUDIT.md`: the milestone state, the status of the items in step 4, and the first traceability gaps.

**Exit:** the audit committed via a PR; the worktree and data copy in place.

### W2 · LLM client and limiter: optimal throughput control (O1, O2)
Implement whatever W1 found missing, in `src/llm/`, each item with tests on the fake server:
1. **Goodput signal:** completed output tokens per second (EWMA over 2 windows of 30 s, with the window length injectable for tests). Requests that fail do not count.
2. **Growth:** while goodput rose by at least 5% after the last increase, multiply the limit by 1.25 (up to the ceiling).
3. **Plateau:** if goodput did not rise by at least 5% after an increase, return to the previous limit and hold 2 windows. Every 10 windows at a plateau, probe once upward (×1.1) to track capacity changes.
4. **Tail guard:** if p95 > 2 × the p95 of the hourly low-load probe window (decision 0005), step back ×0.9 and hold one window.
5. **Hard backoff, unchanged:** 429/503, more than 5% timeouts, or more than 2% connection failures halve the limit and hold one window. The floor is 1 and the ceiling comes from the profile.
6. **Client hygiene:**
   - one curl multi handle per process, with shared DNS and connection caches and TLS session reuse;
   - HTTP/2 multiplexing (`CURLPIPE_MULTIPLEX`) with a cap on concurrent streams per connection;
   - a ramp of new connections (a cap in new connections per second, configurable);
   - DNS cache timeout ≥ the record TTL.
7. **Fake-server scenarios** (extend `tools/fake_deepseek`):
   - `plateau_flat_median`: throughput capped at N in flight, median flat, p99 rising past N;
   - `dns_fail_burst` (or a resolver hook);
   - `connect_fail_burst`;
   - `capacity_shift`: N changes mid-run.
8. **Tests:**
   - all the O1 targets, with simulated time;
   - DNS lookups = 1 per host;
   - the new-connection rate stays at or below the cap;
   - the thread count is constant from 100 to 1,000 in flight;
   - the existing limiter tests still pass, or are updated only where a decision record changes the rule.
9. **Decision record "Limiter v2: goodput control and client hygiene"** (Accepted: the owner already asked for this), and update `DESIGN.md` › Concurrency to match.
10. **Profile rules v2 decision record** (Proposed for the owner): the ceiling for a well-connected desktop is 1,000 with 700–800 recommended (cite §4.2); laptops are set from a measured probe, not the old 32. Implement behind the existing `rules_version` mechanism.
11. **Optional real check** (≤ 300 calls of tiny prompts): run the new client's limiter against real DeepSeek for 10 minutes, capped at 300 in flight. Show it ramps up and holds without errors. Commit the trace.

**Exit:** O1 and O2 shown with test output, and the decision records merged.

### W3 · Correctness hardening outside Agent M's area (C1–C4)
1. **Traceability audit:** go through `DESIGN.md` rule by rule (skip Agent M's sections) and map each rule to tests. Write the missing tests, smallest first. Commit `TRACEABILITY_AUDIT.md`.
2. **Flake hunt:** run the new-tree suite with `ctest --repeat until-fail:50 -j8`, plus TSan for concurrent tests. Fix flakes in your and shared areas only with a root cause (integrity rules). Report flakes in Agent M's area in `COORDINATION.md`.
3. **Fuzzing (C3):** libFuzzer targets in `tests/fuzz/` (clang only, behind a CMake option). Run each for 10 minutes in `opt-nightly.yml`. Every crash becomes a regression test plus a fix.
4. **Crash consistency (C4):** if no kill-point hook exists yet, add a minimal test-only hook (an environment variable naming a commit step, active only in test builds). Kill at every step of: attempt record durable → terminal batch → durable write → store update → notify. Restart and assert the C4 properties. Coordinate in `COORDINATION.md`: Agent M's M7 chaos tools may later reuse the hook.
5. **Real-API smoke (C5)** (≤ 10 calls): the request shape, the usage fields, truncation, a 401 with an invalid key (unbilled).

**Exit:** C1–C5 each with evidence.

### W4 · Cost model and projection (O4)
1. **`data_depot campaign plan`** (or extend the existing cost projection): given a scope, a depth target and the measured parameters, print:
   - expansions per level;
   - input/output tokens (cache-hit and cache-miss input separately);
   - cost at peak and off-peak prices (decision 0004);
   - time at the current request limit and at the measured stable rate;
   - spend per hour.
2. **Inputs** come from measured data in the store (attempt records: tokens per node per level, children kept, atomic share, retries), falling back to clearly labelled defaults when there are no measurements yet.
3. **`docs/agent/opt/COST_MODEL.md`:** the formulas, the parameters, and a projection table for:
   - P01+P06 one more level;
   - the 7 empty phases to today's depth;
   - the full tree to L9.

   Use the current defaults now; refresh after W5.

**Exit:** projections reproducible from one command; tests for the arithmetic.

### W5 · Funnel cost and quality optimization (O3), the biggest saving
Follow the experiment protocol in §7. Real spend stays within §0 and the allocation in §9.3.
1. **Baseline:** pick a fixed, stratified sample of nodes from `DATA_COPY`: 12 nodes (4 TDs, 4 L5, 4 L6), selected by a fixed seed and listed in the report. Run the current funnel on each once, recording every request and response (a "cassette") plus the per-node metrics (§7.3).
2. **Variants**, one factor at a time, on the same sample:
   - **V1** workers' candidates per aspect 100 → 40;
   - **V2** aspects 4–8 → 3–5;
   - **V3** trims 400 → 300 → a single trim to 150;
   - **V4** synthesizer maximum children 100 → 40 (and no padding);
   - **V5** a compact output format (titles as a plain string array; atomic flags as a separate index list);
   - **V6** a prompt structure maximizing the cache-hit prefix (measure the cache-hit ratio from the usage fields);
   - **V7** the light funnel from L6 instead of L8.
3. **Quality judgement:**
   - the deterministic check pass rate;
   - the critique score (same critic, same temperature);
   - duplicate and ancestor-repeat rates;
   - a blind pairwise LLM judge (baseline vs variant children for the same node, randomized order, 1 judge call per pair, prompt fixed in the repo);
   - a 10-node human review sample prepared for the owner as an OA (a side-by-side HTML page, no API calls).
4. **Combine the winners** into one candidate configuration and run it on a fresh sample of 12 nodes to check generalization.
5. **Write `docs/agent/opt/experiments/FUNNEL_OPT.md`:**
   - a table per variant: Δ tokens, Δ cost, Δ quality metrics with 95% confidence intervals;
   - the recommendation;
   - the measured growth factor (children × non-atomic share) per level.
6. **Proposed decision record** "Funnel defaults v2" with the recommended parameters. Implement them behind configuration so the owner's approval is a one-line config change. Do not change defaults until approved.
7. **Refresh `COST_MODEL.md`** with the measured parameters.

**Exit:** O3 met or explained; the proposal and OAs written.

### W6 · Fleet speed-up question (F6), owner-run
Prepare OA "Two-network concurrency test": two machines on different networks (for example, home plus a phone hotspot or a cloud VM), each with its own key, running the probe at 600 in flight simultaneously. Include exact commands, a timing method for a simultaneous start, and how to read the result (sum ≈ 2× one machine → per machine; ≈ 1× → per account). Add a cost estimate.

### W7 · Read-only second opinion on M6 convergence (only after W1–W4)
Read Agent M's M6 code, the test and the worklog without changing anything. Write `docs/agent/opt/reviews/m6-convergence.md`:
- the precise definition of "finished" the test assumes versus what `DESIGN.md` defines;
- candidate liveness bugs (budget-exhaustion detection across nodes, RESERVE fold order, lane ownership of tickets that remain open, quiescence detection), each with the evidence that would confirm or refute it;
- a deterministic reproduction recipe for the in-process fleet harness.

Post a summary line in `COORDINATION.md` and an OA asking the owner to pass it on.

---

## 7. Experiment protocol (W5 and any tuning work)

### 7.1 Discipline
- Same nodes, same model, same temperatures and same critic for every variant. Change one factor at a time.
- Fixed seeds. Every sample, prompt and parameter set committed before running.
- Every real request and response stored as a cassette (JSONL with metadata), so analysis can be redone without new spend. Strip keys and authorization headers.

### 7.2 Budget per experiment
Before each run, estimate the calls and tokens (12 nodes × about 15 calls ≈ 180 calls per variant run). Check against the remaining allocation; abort if exceeded.

### 7.3 Metrics per node
- input tokens (cache hit / miss) and output tokens;
- cost (decision 0004 prices);
- calls;
- wall time;
- attempts and retries;
- children kept;
- atomic count;
- critique score;
- deterministic check failures by type;
- duplicate and ancestor-repeat rejects.

### 7.4 Statistics
- Report means with 95% confidence intervals (bootstrap over nodes).
- A variant "wins" only if its token reduction is significant and no quality metric is significantly worse.
- With only 12 nodes, prefer large effects. State the uncertainty honestly; never overclaim.

### 7.5 Reports
One Markdown file per experiment: hypothesis, setup, results table, conclusion, cassette path, spend.

---

## 8. Session protocol
1. Start in `~/Desktop/onion-opt`.
2. Read `docs/agent/opt/WORKLOG.md` (latest first), `COORDINATION.md`, `OWNER_ACTIONS.md`, then this file's current workstream.
3. Run `git fetch origin && git status && git log --oneline -5 origin/main && gh pr list`.
4. Continue from the **Next step** line of the latest worklog entry.
5. After each task: append a worklog entry (result, evidence, attempts, Next step).

Your memory files live in `docs/agent/opt/`: `WORKLOG.md`, `PROGRESS.md`, `OWNER_ACTIONS.md`, `COORDINATION.md`, `AUDIT.md`, `TRACEABILITY_AUDIT.md`, `COST_MODEL.md`, `experiments/`, `reviews/`, `evidence/`.

---

## 9. Rules

### 9.1 Integrity (from `AUTONOMOUS_GOAL.md` §7, plus)
- Never weaken, skip or delete a test to pass; never add `|| true` or continue-on-error.
- Never report a number without the run that produced it.
- Never present an estimate as a measurement: label estimates.
- Never tune a constant to make a test pass. Constants change only through decision records.

### 9.2 Git and secrets
- Git and secret rules from `AUTONOMOUS_GOAL.md` §6.1–§6.2.
- Explicit staging only, with the secret scan before every commit.
- Plus §2 of this file.

### 9.3 Money and the key
- Caps from §0. Allocation:

  | Use | Max calls |
  |---|---:|
  | W2 real limiter check | 300 (tiny prompts) |
  | W3 real smoke | 10 |
  | W5 baseline + variants + judge | 1,000 |
  | Reserve | 190 |

- Use the key per command, never printed: `DEEPSEEK_API_KEY="$(cat ~/.config/onion/deepseek_key)" <command>` when the environment variable is hidden.
- Log every batch (date, purpose, calls, tokens, cost) in `docs/agent/opt/PROGRESS.md`.
- Never run concurrency above 300 in flight. Never repeat heavy tests back-to-back: F4 showed that this triggers temporary blocks.

### 9.4 Changes to the design
- Clarifications and implementation details: an **Accepted** decision record.
- Constants, defaults, formats, security properties: a **Proposed** decision record, an OA, and the change implemented behind configuration with the current default unchanged.

### 9.5 Hard stops (write `docs/agent/opt/STOP.md`)
- A secret exposed.
- An action that would write outside your worktree, `DATA_COPY` or temp directories.
- An action needing a force-push or a history rewrite on a shared branch.
- An action that would modify Agent M's files or branches.
- Spend exceeding a cap.
- CI unusable for more than 24 h.

### 9.6 Anti-loop
From `AUTONOMOUS_GOAL.md` §8.3 (3 attempts → change strategy; 6 attempts or 4 h → BLOCKED write-up and move on; the same CI failure on 3 pushes counts as a loop).

---

## 10. Definition of done

- [ ] W1 audit merged; worktree, data copy, probe tool and evidence in the repo.
- [ ] O1 and O2 proven by tests; Limiter v2 merged; profile rules v2 proposed.
- [ ] C1–C5 proven: traceability audit, flake hunt, fuzzers in nightly CI, crash-consistency tests, real smoke.
- [ ] O4: the `campaign plan` projection merged, with `COST_MODEL.md`.
- [ ] O3: funnel experiments reported with confidence intervals; "Funnel defaults v2" proposed behind configuration; human-review OA prepared.
- [ ] W6 OA written; W7 review written (if reached).
- [ ] Every PR merged green, no Agent M file touched, every merge noted in `COORDINATION.md`.
- [ ] Real spend within caps, logged.
- [ ] `docs/agent/opt/FINAL_REPORT.md` written:
  - the outcome;
  - each target C1–C5 and O1–O4 with its evidence;
  - the decision records (Accepted / Proposed);
  - the owner actions in recommended order;
  - spend;
  - known limitations.

---

## 11. Report format (end of each workstream, posted in the session)

```
OPT · W<n> <name> · PASS | PARTIAL | BLOCKED
Evidence: commits, PR links, CI runs, command output (last lines)
Targets met: C?/O? with numbers
Decision records: <id> Accepted|Proposed
Owner actions added: OA-…
Real API used: calls / tokens / USD (running total vs caps)
Coordination notes: files touched in shared areas, notices posted
Next: W<n+1> first step
```

---

## 12. Kickoff message (owner → new agent)

> You are the second engineer on this project; another agent is building M6 at the same time. Your instructions are in `OPTIMIZATION_GOAL.md`, which I'm giving you now. Read it completely first, especially §2 (coordination), then `docs/DESIGN.md`, `docs/decisions/` and `docs/agent/AUTONOMOUS_GOAL.md` §6–§9.
> Work only in your own worktree, `~/Desktop/onion-opt` (create it per §2.1), and never in `~/Desktop/onion`. As your first PR, add this file as `docs/agent/opt/OPTIMIZATION_GOAL.md`.
> Work autonomously through W1 to W7 toward the definition of done in §10. Don't ask me for approval; record what needs me in `docs/agent/opt/OWNER_ACTIONS.md`. Start with W1.

---

## 13. Coordination note for Agent M (owner → M6 agent)

> FYI: a second agent ("OPT") now works in parallel, in its own git worktree (`~/Desktop/onion-opt`) on branches named `opt/*`. It won't touch `src/sync/`, `src/swim/`, the lane and stealing parts of `src/sched/`, `tests/fleet/`, `tools/chaos/`, your progress files or `ci.yml`. It may merge small PRs into `main` (LLM client and limiter, tests, fuzzing, cost projection, docs under `docs/agent/opt/`).
> Please:
> 1. Never check out or modify `opt/*` branches.
> 2. Read `docs/agent/opt/COORDINATION.md` at the start of each session; it lists OPT's merges and any notes for you.
> 3. Merge `origin/main` into `m6-sync` when a note says your area is affected, and before you open your M6 PR.
> 4. Take the next free decision-record number on `origin/main` right before committing one.
