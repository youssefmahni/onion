# Onion Engine — Autonomous Goal Brief (M2 finish → M9)

> **For Youssef, before handing this over**
> 1. Check §0. Real DeepSeek calls are pre-approved, within the caps there.
> 2. Give the agent the kickoff message at the end of this file (§15). Its first commit puts this file into the repo as `docs/agent/AUTONOMOUS_GOAL.md`.

---

## 0. Owner settings

| Setting | Value | Meaning |
|---|---|---|
| `REAL_CALLS_ALLOWED` | `yes` | You may make real DeepSeek calls within the caps below and the per-milestone allocation in §6.3 |
| `REAL_CALL_CAP` | `300` | Maximum real completion calls for the entire run (`GET /models` is free and does not count) |
| `REAL_USD_CAP` | `3.00` | Maximum real spend for the entire run, in USD |
| `KEY_SOURCE` | `DEEPSEEK_API_KEY`, else the file `~/.config/onion/deepseek_key` | Where the key comes from. See §6.3 for how to use it without ever printing it |
| `CORPUS_COPY` | `~/onion-data` | A copy of the corpus you may read and write. Never touch any other copy |
| `ORIGINAL_PROJECT` | `<fill in, or leave blank>` | Folder of the old project, used only to make a fresh corpus copy with `legacy/m0_backup.sh` |
| `MERGE_POLICY` | `agent-merges` | You merge your own milestone PRs into `main` once the merge checklist (§9.4) passes |

---

## 1. The goal

Bring the Onion Engine rebuild from its current state (M2 nearly finished) to **agent-complete for every milestone M2 to M9**, as defined in §12. Work continuously.

Do not stop for approvals. This brief pre-approves your normal work, including merging your own milestone PRs.

You stop only when:
- **(a)** the goal in §12 is met;
- **(b)** a hard-stop condition in §8.1 occurs; or
- **(c)** every remaining task depends on a blocked item (§8.3).

Things only the owner can do (physical machines, real money beyond the caps, the offline admin key, approving dropped data, deleting `legacy/`) are **not** reasons to stop. Prepare each one completely as a runbook in `docs/agent/OWNER_ACTIONS.md` (§10), then continue with the next task.

---

## 2. Sources of truth

| Order | Source | Role |
|---|---|---|
| 1 | The owner's messages in the session | Override everything |
| 2 | `docs/DESIGN.md` | Architecture, every rule and constant. Authoritative |
| 3 | `docs/decisions/0000–0006` and later records | Clarifications already approved. Binding |
| 4 | `docs/REBUILD_PLAN.md` | Milestone scope and exit gates |
| 5 | `docs/agent/IMPLEMENTATION_BRIEF.md` §5–§9 | Spec digest, layout, interface sketches, test harness specs |
| 6 | This file | Process, current status, plan |

**This file replaces the process rules of `IMPLEMENTATION_BRIEF.md`:**
- the stop-at-every-gate rule in §4.1;
- the kickoff in §13;
- the per-milestone approval waits.

Every other rule in that brief still applies (secrets, data safety, dependencies, portability, determinism, tests first).

If `DESIGN.md` and anything else disagree, `DESIGN.md` wins. Record the disagreement in `docs/agent/WORKLOG.md`.

---

## 3. Verified history and current status

### 3.1 How the project got here

- **M0**
  - The P1–P7 fixes were applied to the legacy code: temp files, macOS sampler, duplicate paths, truncated replies, timeout, circuit breaker, commit after the quality decision.
  - Proven against a fake server over real HTTP.
  - The **real DeepSeek run on a corpus copy has never been confirmed to the owner's reviewer**.
- **M1** is complete, merged as PR #1 (`b885e96`) and tagged `m1`. It includes:
  - the new layout;
  - CMake presets;
  - CI on Linux, macOS arm64, Windows, and an Intel macOS 12.0 compatibility build (with `lipo`/`otool` proof);
  - 67 core + 8 GPU tests copied;
  - pinned dependencies;
  - the platform layer (durable writes with file and directory sync, `F_FULLFSYNC` fallback, atomic replace, data-directory precedence, samplers per OS, key store);
  - golden vectors (RFC 8032, BLAKE2b, RFC 8785 subset, multilingual signed batch, HLC above 2^53).
- **A legacy test race** (`PoolResizesFromControlFile`) was fixed test-only in PR #2.
- **M2 (branch `m2-deepseek-client`)** has:
  - the fake DeepSeek server;
  - the limiter with the probe-only baseline (decision 0005);
  - the libcurl-multi client using `curl_multi_poll` with stable heap ownership;
  - HTTP/2 preference with HTTP/1.1 fallback;
  - a CPU pool (14 threads at both 100 and 1,000 in flight);
  - integer-decimal budget accounting with reservations;
  - timeout spend recorded as uncertain at the full reservation;
  - the error-outcome contract and breaker (decision 0006);
  - the file-descriptor ceiling;
  - ASan/UBSan and TSan CI jobs;
  - `data_depot llm bench`.

**Earlier M2 bugs, all fixed:**
- a use-after-free (a deque iterator kept after unlock), found by ASan;
- a construction-order race, found by TSan;
- port 9 being filtered on Windows, which exposed that timeouts never opened the breaker.

### 3.2 Contradictions and gaps you must resolve first

The latest status report (`status_repport.md`, HEAD `9006166`) conflicts with earlier reports, or omits items. Verify each one with commands before doing anything else, and record what you find in `WORKLOG.md`.

| # | Issue | What to verify or do |
|---|---|---|
| G1 | **The `m0` tag.** An earlier report said `m0` exists on `42ecd8a` with a "verified run" annotation; the status report says `m0` is not tagged and the real run is pending | Run `git tag -l m0` and `git ls-remote --tags origin`, and read the annotation if the tag exists. Either way, treat the real run as **unconfirmed**: close it per §11 M0 or as an owner action. Never create or move `m0` without a real run you performed and logged |
| G2 | **The macOS arm64 new-tree job is missing** from the report's list of jobs (green or failing). The macOS ≥ 4,096 file-descriptor assertion lives in that job | Check `.github/workflows/` and the latest run. The job must exist, run on PRs, and pass, and the assertion must actually execute there (quote its log line) |
| G3 | **Windows `llm.closed_port_1000` fails**: 1,000 refused-port attempts end as Timeout at the 2 s deadline, with zero connections | See §3.3 for the probable cause. Keep the `Transport` semantics; never accept Timeout for a refused port |
| G4 | **Adaptive limiter traces are missing** from the benchmark evidence (`healthy`, `rate_limited`, `slow_under_load`) | Required for the M2 gate. Produce them as committed CSV plus a summary (§11 M2) |
| G5 | **The pinned Windows curl is 8.11.1** (December 2024): network-facing and about two years old | Update the pin to the latest curl release that is at least 14 days old. Verify the SHA-256 from curl.se, and update `VERSIONS.md` and decision 0002 |
| G6 | **Windows has no HTTP/2** (no nghttp2 in the vendored curl) | Accepted limitation (owner decision). Keep HTTP/1.1 there, keep it recorded, and do not add nghttp2 |
| G7 | **Stray files at the repo root:** `status_repport.md` (misspelled) and `onion.task` | Move them to `docs/agent/STATUS.md` and fold `onion.task` into `WORKLOG.md`, in one commit on your current branch |
| G8 | **Agent-memory files may be missing**: `WORKLOG.md`, `OWNER_ACTIONS.md`, `TRACEABILITY.md` | Create them (§9.2) |
| G9 | **The half-budget run** shows 500 started and 500 refused. That proves refusal, but not the engine's behaviour, where a ticket that doesn't fit waits rather than being refused forever | Keep the run; add in M3 a test that waiting tickets start once reservations are released |

### 3.3 Diagnosis lead for G3 (Windows refused port)

On Windows, connecting to a closed localhost port does not fail immediately. The Windows TCP stack retries the connection attempt after receiving a reset, and the refusal (`WSAECONNREFUSED`) typically arrives after about 2 seconds. With a 2 s deadline, curl times out first. That matches "1,000 Timeouts, zero connections".

**How to confirm:**
1. On the Windows runner, make one attempt to a reserved-then-released loopback port with a 10 s deadline, and log the time until refusal.
2. If refusal arrives after about 2 s, set that test's deadline to 10 s (or separate the connect timeout from the total timeout) and keep requiring `Transport` for all 1,000.
3. If it never refuses, report the measured behaviour and treat it as a blocker. Do not relax the assertion.

**Production note:** use separate `CURLOPT_CONNECTTIMEOUT` and total timeouts, so a slow refusal on Windows is classified correctly in real use too.

### 3.4 Lessons from the previous agent (they are now rules)

| Lapse | Rule |
|---|---|
| Committed milestone work directly to `main`, and reported the wrong branch | §9.1: verify the branch before every commit; quote `git log` in reports |
| Staged with `git add -A` while an untracked `.aws` folder sat in the repo | §6.2: stage explicit paths only |
| Changed a failing test instead of questioning the behaviour (port 9 hid the timeout-breaker gap) | §7: never weaken a test; a failing test is first evidence about the product |
| Reports said "done" without the requested numbers | §9.3: every claim carries evidence |
| Stopped after every small step | §1: continue until the goal or a real stop condition |

---

## 4. Environment

| Item | Value |
|---|---|
| Repo | `~/Desktop/onion`, remote `git@github.com:youssefmahni/onion.git`, default branch `main` |
| Development machine | Kali Linux, GCC 16.2, CMake, Python 3, POCL (set `POCL_CACHE_DIR` to a writable directory) |
| Tools | `gh` is authenticated for this repo only. Use it for PRs, CI runs, logs and merges |
| CI | Linux (normal, ASan+UBSan, TSan), macOS arm64, macOS x86_64 / 12.0 build, Windows, legacy Linux, legacy macOS |
| Fleet hardware (owner-only) | 3 Mac Pros (Intel, macOS 12, FirePro D500), future Linux and Windows 10/11 machines |
| Sandbox caveats | Loopback binding may be restricted in your sandbox. Tests needing it must still pass in CI; note any that can only run there |

---

## 5. Session protocol (your memory lives in the repo)

Your context may be cut or compacted at any time, so the repo is your memory.

**At the start of every session, and after any context loss:**
1. Read `docs/agent/WORKLOG.md` (latest entries first), `docs/agent/PROGRESS.md` and `docs/agent/OWNER_ACTIONS.md`, then the section of this file for the current milestone.
2. Run `git status`, `git branch --show-current`, `git log --oneline -5` and `gh run list -L 5`.
3. Continue from the **Next step** line of the latest worklog entry.

**After every task, and before any long wait:** append a worklog entry (§9.2) that ends with a precise **Next step** line.

---

## 6. Non-negotiable rules

### 6.1 Git
- **Branches.** One branch per milestone: `m2-deepseek-client` (exists), then `m3-engine`, `m4-signed-log`, `m5-profiles-service`, `m6-sync`, `m7-dynamic-fleet`, `m8-depth-scale`, `m9-hardening`. Each new branch starts from the freshly updated `main`.
- **Never commit to `main`.** Never force-push any shared branch. Never rewrite pushed history.
- **Merging.** Merge only through a PR, with a merge commit (`gh pr merge --merge`), after the merge checklist in §9.4 passes.
- **Tags.** After merging milestone N, tag `mN-rc` on the merge commit. Only the owner creates the final `mN` tags, after the live checks.
- **Pushing.** Push coherent units of work, not every keystroke, because CI minutes are limited.

### 6.2 Secrets
- **Never** read, print, log, store or commit an API key, a private key, or the contents of `.aws/`, `.env` or `*.pem`.
- Stage explicit paths only, never `git add -A` or `git add .`.
- Before every commit, run:
  ```bash
  git diff --cached | grep -nEi 'sk-[a-z0-9_-]{16,}|BEGIN [A-Z ]*PRIVATE KEY|(api[_-]?key|secret|token)[^\n]{0,8}[:=][^\n]{0,4}[A-Za-z0-9_-]{20,}' && echo STOP || true
  ```
  If it prints anything other than `true`'s silence, that's a hard stop (§8.1).
- Keys reach the program only through the environment or the OS key store, never through files in the repo or service definitions.

### 6.3 Money and the real API key

**Getting the key without exposing it.**
- Check for it without printing it:
  ```bash
  if [ -n "$DEEPSEEK_API_KEY" ]; then echo env; elif [ -s ~/.config/onion/deepseek_key ]; then echo file; else echo missing; fi
  ```
- Some agent tools hide environment variables whose names contain KEY, SECRET or TOKEN from the agent's shell (Codex does this by default). If the variable is hidden but the file exists, pass the key per command only:
  ```bash
  DEEPSEEK_API_KEY="$(cat ~/.config/onion/deepseek_key)" <command>
  ```
- **Never** echo, log or copy the key, and never let it reach the repo, a test fixture, a CI secret or a service file.
- If both sources are missing, every real-call step becomes an owner action.

**Caps.** Stay within `REAL_CALL_CAP` and `REAL_USD_CAP` for the whole run, and within this allocation:

| Use | Max calls | Notes |
|---|---:|---|
| M0 real legacy node | 15 | One TD on the corpus copy |
| M2 API validation | 5 | Request shape, usage fields, cache hits, truncation, HTTP version (plus a free 401 check) |
| M2 real concurrency probe | 100 | Tiny prompts, `max_tokens` ≤ 16 |
| M3 real campaign | 150 | At most 10 nodes, light settings |
| Reserve | 30 | Re-runs after a fix; anything else |

**Logging and limits.**
- Log every batch in `PROGRESS.md` under "Real API usage": date, purpose, calls, cost computed from the usage fields and the prices in decision 0004.
- Before each batch, check the remaining allocation.
- If a step would exceed its allocation or a cap, don't run it; record it as an owner action and continue.
- A run of more than 10 consecutive non-OK real responses ends that batch immediately.

### 6.4 Data
- Read and write only the repo, temp directories you create, and `CORPUS_COPY`.
- Never touch the live corpus, any Mac Pro data directory, or anything else outside those.
- Print and check any path before a destructive command; never `rm -rf` a variable or wildcard you haven't printed.

### 6.5 Legacy
- `legacy/` stays unchanged. The only exception is a test-only fix for a proven flake, made on its own branch and PR into `main`.
- Never link new code to legacy code.
- Deleting `legacy/` is prepared as a PR in M9 but never merged by you.

### 6.6 Dependencies
- **Allowed:** libcurl, cpp-httplib, Monocypher (+ Ed25519), the SQLite amalgamation, zstd, nlohmann/json, the OpenCL headers, GoogleTest. Plus, for fuzzing in M9, libFuzzer, which comes with clang (a toolchain feature, not a new dependency).
- **Not allowed without the owner:** anything else (DuckDB, nghttp2, any new library). Record it as an owner action and continue without it.
- **Every pin:** an exact version at least 14 days old, its SHA-256, its license, recorded in `third_party/VERSIONS.md`.

### 6.7 Portability and determinism
These rules from the implementation brief §4.8 and `DESIGN.md` apply unchanged:
- OS calls only in `src/platform/`;
- LF line endings and UTF-8 everywhere;
- no floats in records (decision 0003: signed 64-bit integers as exact decimal; scores and money as decimal strings);
- `std::to_chars` for numbers;
- no new warnings.

---

## 7. Integrity rules (never fake progress)

You are measured by working software, not by green checkmarks. You must **never**:

1. Delete, skip or disable a test, or mark one flaky, to make CI pass.
2. Loosen an assertion (accept another outcome, widen a tolerance, lower a count) unless a decision record shows that the new expectation is what `DESIGN.md` specifies. Pure test mistakes (a wrong fixture or a race in the test itself) may be fixed, with the root cause stated.
3. Add `continue-on-error`, `|| true`, retry-until-pass loops or `--repeat until-pass` to CI or test commands.
4. Reduce sanitizer coverage, compiler warnings or CI jobs.
5. Put test-only shortcuts in production code paths (fake clocks and hooks must be injected through interfaces or test builds only).
6. Report a number, a pass or a "done" without the command output, CI link or commit hash that proves it.
7. Change a constant, format, security property or conflict rule in `DESIGN.md` on your own (see §8.2).

A failing test is evidence about the product first. Before touching a test, write down in `WORKLOG.md` why the product is right and the test is wrong.

---

## 8. Stop, escalation and anti-loop rules

### 8.1 Hard stops (stop immediately)

Stop and write `docs/agent/STOP.md` with the facts and the exact state if:
- a secret is found in git history, in a staged change or in CI logs;
- an action would touch data outside §6.4, or lose data;
- an action needs a force-push or rewritten history on a shared branch;
- CI cannot run at all (minutes exhausted, billing block, runner unavailable for more than 24 h);
- a real call would exceed the money caps while that same step is on the critical path for everything remaining.

### 8.2 Owner decisions (don't stop: park and continue)

If the spec is silent or contradictory on something that changes a constant, a format, a security property or user-visible behaviour:
1. Write a decision record with status **Proposed** (context, options, your recommendation).
2. Add an entry to `OWNER_ACTIONS.md` asking for that decision.
3. Implement the most conservative option behind a clearly named setting, if possible, or implement the spec as written.
4. Continue.

Pure clarifications that don't change behaviour (naming, file locations, test fixtures) you decide yourself, with an **Accepted** decision record.

### 8.3 Anti-loop rules

Every problem (a failing test, a CI job or a design question) gets an entry in `WORKLOG.md` with numbered **attempts**. Each attempt records a hypothesis, the change, and the evidence.

| Situation | Action |
|---|---|
| Three attempts on one problem, all failing | Change strategy: shrink the reproduction to the smallest case, read upstream docs and issues, add instrumentation. No more fix attempts without new evidence |
| Six attempts or 4 hours of wall-clock time on one problem | Mark it **BLOCKED**: a full write-up (symptom, evidence, hypotheses tried, what would unblock it) in `WORKLOG.md` and `OWNER_ACTIONS.md`. Move to independent work |
| The same CI job fails with the same first error on 3 consecutive pushes | Treat it as a loop: apply the BLOCKED procedure |
| You are about to revert a change you made earlier for the same problem | Stop and write down why. Oscillating between two states is a loop |
| You catch yourself polling CI repeatedly | Use `gh run watch <id>` with a timeout, and work on something independent meanwhile |
| **Every** remaining task depends on BLOCKED items | Stop with condition (c) in §1, and write the final report (§13) with the blockers first |

Never mark something BLOCKED just because it's hard. BLOCKED means you lack information, access or authority that you cannot obtain.

---

## 9. The work loop

### 9.1 For every task
1. `git branch --show-current`: confirm the milestone branch.
2. Write or extend the test first, from the `DESIGN.md` rule. Watch it fail.
3. Implement until it passes locally:
   - the normal build;
   - ASan+UBSan for any C++ that touches memory, threads or parsing;
   - TSan for concurrent code.
4. Commit with explicit staging, after the secret check (§6.2). Message format: `mN: <what changed>`.
5. Add the rule-to-test mapping to `docs/agent/TRACEABILITY.md`.
6. Append to the worklog. Push when a coherent unit is done, then check CI.

### 9.2 Memory files (create them if missing)

- **`WORKLOG.md`** (append-only). Each entry:
  ```
  ## <UTC time> · <milestone> · <task>
  Result: done | in progress | blocked
  Evidence: <commit hash, command + last lines of output, CI link>
  Attempts: <only for problems: n. hypothesis → change → evidence>
  Next step: <one precise action>
  ```
- **`PROGRESS.md`**: the current milestone, branch, last green CI run, test totals per job, real API usage, and known limitations.
- **`TRACEABILITY.md`**: a table of `DESIGN.md` section / rule → test name(s) → status. Every rule touched by a milestone must have at least one test before that milestone merges.
- **`OWNER_ACTIONS.md`**: see §10.

### 9.3 Evidence standard

Every claim in a report or gate file needs at least one of:
- a commit hash (verified with `git log`);
- a CI run link with job names;
- a command with the last lines of its output.

Numbers come from a run, never from memory.

### 9.4 Merge checklist (before `gh pr merge --merge`)
- [ ] All CI jobs green on the PR head, listed with passed / skipped / failed counts.
- [ ] The milestone's agent-exit criteria (§11) are met, each with evidence.
- [ ] `TRACEABILITY.md`: every rule in this milestone's `DESIGN.md` sections has a test.
- [ ] `DESIGN.md` matches the code; any deviation has a decision record.
- [ ] The gate report `docs/agent/gates/MN.md` is written: Agent-verified results, plus an **Owner verification** checklist with exact commands.
- [ ] Owner actions for this milestone are recorded as runbooks.
- [ ] No secrets, no stray files, no TODOs without an issue reference.

After the merge: tag `mN-rc` on the merge commit, push the tag, update `main` locally, create the next branch, and add a worklog entry.

---

## 10. `OWNER_ACTIONS.md` format

Each entry must be executable by the owner without asking you anything:

```
### OA-<n> · <title> · <milestone> · status: open | done
Why: <what it proves or unblocks>
Prerequisites: <hardware, key, files>
Steps: <exact commands, copy-paste ready, with expected output>
Pass criteria: <what output means success>
If it fails: <what to collect and send back>
Cost / duration: <real calls and USD, wall-clock time>
```

Expected entries include:
- the M0 real node;
- the M2 real-call validation (if not done by you);
- creating a Mac Pro self-hosted runner;
- the M3 real small campaign;
- the M4 admin key generation, duplicate-drop approval and cutover;
- the M5 Mac Pro profile and tuning run;
- the M6 24 h three-Mac soak;
- the M7 Linux join and power pull;
- the M9 Windows joins and mixed soak, approving the `legacy/` deletion PR, and the `v1.0` tag;
- any Proposed decision records.

---

## 11. The plan to the end

Do the milestones in order; within one, follow the task order. Each milestone ends with its agent-exit criteria, then the merge checklist (§9.4).

### Bootstrap (do first, on `m2-deepseek-client`)
1. Read the documents in §2. Resolve G1–G9 (§3.2) with evidence. Do G7 and G8 in one commit.
2. Put this file into `docs/agent/AUTONOMOUS_GOAL.md`.

### M0 · Close the legacy real-node check
- **If the key is available** (§6.3):
  1. If `CORPUS_COPY` lacks `Onion - level_04.csv`, create a copy from `ORIGINAL_PROJECT` with `legacy/m0_backup.sh`. If that path is blank, make it an owner action.
  2. Run `GET /models` (free) and record the model ids.
  3. `python3 legacy/m0_clean_failure_rows.py $CORPUS_COPY` (preview only).
  4. Choose one TD with no `funnel_state.tsv` row.
  5. Run `legacy/build/data_depot funnel --root $CORPUS_COPY --root-node <TD> --limit 1` with `OPENROUTER_API_KEY` and `DEPOT_API_URL` unset. Use `--model <id>` if the default id isn't listed.
  6. Pass means `NODE_DONE … FINAL` and `errors=0`. Record the `DUP_PATHS` count and the calls used.
  7. On a pass, if `m0` doesn't exist, create it on `42ecd8a` with the annotation "M0: P1-P7 fixed, one real DeepSeek node verified (agent run <date>)". If it exists, leave it and record your run.
- **If the key is missing:** write OA "M0 real node" with these exact steps.

### M2 · DeepSeek client: finish
1. **G3:** confirm the cause by measurement on Windows, separate connect and total timeouts in production code, fix the test deadline, keep `Transport`.
2. **G5:** update the Windows curl pin.
3. **G2:** prove the macOS arm64 job runs the ≥ 4,096 file-descriptor assertion.
4. **G4, adaptive traces:** limiter on (start 16, ceiling 2,000), against `healthy`, `rate_limited` and `slow_under_load`, with a compressed window length. Commit the CSV traces under `docs/agent/evidence/m2/` with a summary:
   - growth while healthy;
   - halving plus a one-window hold on 429;
   - holding while latency rises;
   - recovery;
   - the hourly probe capping and then restoring the limit.
5. **Re-run the benchmark set** on the final code and commit the results under `docs/agent/evidence/m2/`:
   - capacity at limit 1,000: peak in-flight, throughput, latency percentiles, threads;
   - connection reuse at limit 100;
   - threads at 100 vs 1,000 in flight;
   - the half-budget run;
   - the timeout outage;
   - the 401 and 402 breaker runs;
   - the 429 storm.
6. **Real-call validation** (at most 5 completions, plus the free checks):
   - `GET /models`, confirming the model id against decision 0004;
   - one minimal completion with `reasoning_effort: "none"`, recording the accepted request shape, the usage field names (cache-hit and cache-miss input, output), `finish_reason` and the negotiated HTTP version;
   - the same prefix again, to observe cache-hit tokens;
   - one call with a tiny `max_tokens`, to see `finish_reason: "length"` classified as Truncated;
   - one call with a deliberately invalid key, expecting 401 and the breaker's authentication path (not billed).

   If DeepSeek rejects any field (for example `reasoning_effort`), stop the batch, fix the request shape, and record it in a decision record. Update decision 0004 and the client's field mapping with the findings.
7. **Real concurrency probe** (at most 100 calls; tiny prompts, `max_tokens` ≤ 16). This measures DeepSeek's real behaviour before M3 depends on it, as the rebuild plan's risk table asks:
   - send 100 requests with the adaptive limiter on, starting at 4, ceiling 50;
   - record per 30-second window: the limit, latency p50/p95, errors by kind (especially 429/503), connections opened, and HTTP version;
   - commit the trace and a summary under `docs/agent/evidence/m2/real_probe.*`;
   - if DeepSeek returns 429 or 503, confirm the limiter halved and the breaker stayed closed.

   If the key is missing, record steps 6–7 as an OA with exact commands.
8. **Agent-exit criteria:**
   - all CI jobs green;
   - every M2 row of the rebuild plan's gate shown with evidence: 1,000 in flight on a bounded thread count, the limit halving on 429, no budget overspend;
   - the real-call findings and the concurrency probe applied (or the OAs written);
   - `TRACEABILITY.md` covers `DESIGN.md` › LLM provider, keys and budgets, and › Concurrency.

### M3 · One-machine engine (branch `m3-engine`)
1. **`src/store`:** vendored SQLite (pinned), the schema exactly as in `DESIGN.md` plus `schema_version` and migrations, WAL mode, the subtree query, and CSV export up to L9.
2. **`src/state`:** the single state-writer thread with one queue. Nothing else writes SQLite. Group-commit `ATTEMPT` records with a flush at least once per second (decision 0000).
3. **`src/log` part 1:**
   - `Record` and `Batch` types;
   - canonical bytes per decision 0003;
   - segment append with durable sync;
   - a `Signer` interface with a `NullSigner`.

   The commit order is real from here on: `ATTEMPT` durable per attempt; the terminal batch (`ADD_NODE`…, `CRITIQUE`, `ATOMIC`, `SETTLE` last) or `NEEDS_REVIEW`; then the SQLite update; then notices.
4. **`src/gate`:**
   - port the five agent prompts from `legacy/depot_io.cpp`, restructured with fixed text first and node text last;
   - add the pre-check prompt (at most 400 tokens) and the light funnel from the campaign's depth;
   - implement the deterministic checks as pure functions:
     - a complete reply;
     - valid JSON;
     - 3–100 children;
     - canonical-title repeats against the node, siblings and existing children;
     - ancestors at ≥ 0.85 for the parent and ≥ 0.95 above it;
     - the style gates;
   - implement `decide()` as a pure function: pass at ≥ 0.7; up to 3 attempts at temperatures 0, 0.3 and 0.5, with the previous issues injected; leaf when no attempt has 3 distinct valid children; best attempt at ≥ 0.5; otherwise `NEEDS_REVIEW`;
   - atomic verification as a short separate call;
   - the quality guard (rolling score and failure rate vs the fleet median; locally, a fixed threshold).
5. **`src/sched` (single device):**
   - the campaign JSON file and its decision record;
   - `start` / `pause` / `resume` / `stop` (stop is final);
   - the cost projection before start (the 7-day average, or a 20-attempt calibration against the configured endpoint) showing both peak and off-peak prices (decision 0004), and the projected log growth;
   - the ticket index;
   - breadth-first and depth-first order, with ties broken by path;
   - loop-budget chunks (`RESERVE`, default 50);
   - the stop rule;
   - reservation waiting: a ticket that doesn't fit waits and starts after a release (G9);
   - the review queue with batch actions, priority by branch spend, and optional auto-accept.
6. **Development import:** an unsigned CSV/TSV → SQLite loader for development, clearly separated from the M4 genesis import.
7. **`src/cli` and `src/api`:**
   - `node run`;
   - `campaign start|pause|resume|stop`;
   - `review list|accept|retry|reject`;
   - `llm bench`;
   - `/api/state` with campaigns, reviews, spend and lanes (one lane);
   - POST endpoints requiring a per-device token, a matching `Origin` and `Content-Type: application/json`;
   - bound to 127.0.0.1 only.
8. **`web/dashboard.html`:** adapted from legacy to the new `/api/state`, with a campaign panel and a review queue that has buttons.
9. **Fake server, `funnel` scenario.** Deterministic replies for every agent prompt, keyed by node path, so whole campaigns run offline. It must be scriptable to produce:
   - low scores, to force retries;
   - cannot-split nodes;
   - truncation;
   - ancestor repeats;
   - review cases.
10. **Tests:**
    - one per gate row;
    - the P7 rule (nothing committed before the decision);
    - restart resumes at attempt n+1 with stored issues;
    - spend recorded across a crash;
    - review flows;
    - stop is final;
    - API rejection cases;
    - waiting reservations.
11. **Agent-exit criteria:**
    - a campaign of at least 300 nodes on one machine against the `funnel` scenario;
    - no failed attempt committed (verified by scanning the log);
    - review works through the API, and through the dashboard by an automated browser-free check (HTTP calls plus HTML presence);
    - the cost projection shown before start;
    - a kill at each commit step followed by a restart loses no committed work;
    - CI green.

    Then a **real campaign** of at most 10 nodes within the M3 allocation (§6.3): one TD from the corpus copy, light settings. Report child counts, scores, retries, cost per node, and projected vs actual cost. If the key is missing, an OA.

### M4 · Signed log and genesis import (branch `m4-signed-log`)
1. Device keys (Monocypher Ed25519) through the `KeyStore`, `device_id`, the slot, 64-bit ids (`TK%05lld`), and the HLC as a monotonic integer (decision 0003).
2. Replace `NullSigner`:
   - batch signing over `prev ‖ hash`;
   - batch 1 chained from the public-key hash;
   - verification with every rejection case (bad signature, bad `prev`, bad hash, gap, non-member, past cutoff, unknown version);
   - quarantine.
3. **Segments:** sealed at 64 MB, zstd-compressed (pinned), torn-tail repair on open, and a test cutting at **every byte offset** of the last batch.
4. **Checkpoints:** `VACUUM INTO`, a manifest (version vector, Merkle root over `nodes` sorted by path, with the leaf encoding in a decision record), one signature plus support for a co-signature, keeping the last 3.
5. **`data_depot admin keygen --out <path>`:** refuses any path inside the data directory and prints the public key. The genesis-time admin public-key configuration goes with it.
6. **`tools/import`:**
   - the genesis import from a corpus copy, with legacy ids in slot 0 and settles, critiques and atomic flags marked `legacy`;
   - a **duplicate-path proposal file**, listing per path the candidate titles and a recommended keeper with the reason. The import refuses to run until an owner-approved resolution file exists;
   - the verifier: counts per layer, every parent present, the Merkle root against a fresh CSV read.
7. **Run the import on `CORPUS_COPY` only.** Commit the verifier report (counts, duplicate count, Merkle root) under `docs/agent/evidence/m4/`, never data.
8. **A weekly dry-run script**, plus `docs/CUTOVER.md`, following `REBUILD_PLAN.md`: parallel run, freeze, clean, import, verify, co-sign, switch, rollback.
9. **Agent-exit criteria:** the import verified on the copy; the torn-tail test at every offset; a lone eviction signature rejected while an admin-signed one is accepted (unit level); CI green.

   **OAs:** generate the admin key offline; approve the duplicate resolutions; perform the cutover with the runbook.

### M5 · Profiles and service (branch `m5-profiles-service`)
1. **`src/profile`:**
   - the scan (OS, CPU, RAM, disk, durable-write latency, OpenCL fp64, battery, link speed, round-trip time to `/models`), with no tokens spent;
   - rules version 1 from `DESIGN.md`;
   - `node.conf` overrides that always win;
   - `MEMBER_PROFILE` versions with `prev`, `reason` and `changed`;
   - refreshes at join, at start, after a reconnect, and every 48 h;
   - `node profile show|history|diff`;
   - battery sampling on macOS (IOKit power sources) and Linux (`/sys/class/power_supply`), deferred from M1.
2. **Key stores:**
   - the macOS Keychain backend;
   - DPAPI verified under a Windows service account;
   - `node key set` to store the DeepSeek key in the OS store; the environment variable becomes a development fallback only.
3. **Services:**
   - a systemd unit: `StateDirectory=onion`, `Restart=always`, no `Environment=` secrets; validate it with `systemd-analyze verify` in CI;
   - a launchd plist with `KeepAlive`; validate it with `plutil -lint`;
   - a Windows Service with recovery set to restart; install, start, kill and verify the restart, then uninstall, all in Windows CI;
   - `node install-service|start|stop|status`.
4. The profile values feed the limiter (start value, ceiling, RAM floor, battery cap).
5. **Agent-exit criteria:** profile fixtures per OS; golden service files; the Windows CI service restart test; the Linux CI restart after `kill -9` (systemd if available on the runner, otherwise process-supervisor equivalence documented). **OA:** Mac Pro profile check and live tuning run.

### M6 · Sync between known machines (branch `m6-sync`)
1. `members.json` as the static member list (this milestone only).
2. **`src/sync`:**
   - cpp-httplib (pinned) on TCP 7947, bound to the LAN interface (configurable, loopback in tests);
   - Ed25519-signed requests (method, path, body hash, timestamp within ±60 s) with a nonce cache;
   - every endpoint in `DESIGN.md` › Sync protocol;
   - 8 MB response caps;
   - vector comparison, pulls (sealed segments whole, the active range as batches), verification, quarantine, durable append and fold;
   - co-signed checkpoints, new-task notices and anti-entropy over Merkle trees with 1,000-record leaves.
3. **`src/sched`:** home lanes by HRW (`score = −w / ln(h)`, `h = (x + 0.5) / 2^64`, `x` = the first 64 bits big-endian of BLAKE2b-256(path ‖ device_id)), and `RESERVE` chunks across machines with void-past-budget.
4. **Fleet harness:** several nodes in one process over an in-memory transport that can drop, delay, duplicate, reorder and partition.
5. **Multi-process test:** three real node processes on one Linux machine with separate data directories and ports, run in CI.
6. **Agent-exit criteria:**
   - harness convergence (identical Merkle roots) across all fault scenarios;
   - the multi-process run converges;
   - budget overshoot at most one chunk per node;
   - every row of the conflict table tested in both orders;
   - CI green.

   **OA:** the 24 h soak on three Mac Pros.

### M7 · Dynamic fleet (branch `m7-dynamic-fleet`)
1. **`src/swim`:**
   - UDP 7946 through an injectable transport;
   - a 1 s period, a 500 ms ack, 2 indirect probes, SUSPECT, refutation with incarnation + 1, DEAD after 5 min (configurable in tests);
   - piggybacked gossip of live values;
   - the signed multicast beacon every 5 s (group address in a decision record);
   - all membership states.
2. **Joining:**
   - `node invite --pubkey --ttl` (single use, nonce recorded);
   - `node join --token` with seed addresses;
   - the slot-collision check;
   - clone from a checkpoint with Merkle verification;
   - ACTIVE once the lag is under 100 records.
3. **Majority and eviction:**
   - the majority rule for DEAD lanes;
   - eviction-signature collection;
   - admin-key eviction;
   - auto-proposal after 30 days absent;
   - `node stop --leave`.
4. **Stealing:**
   - only when short of work;
   - from the far end, at random among the top 5;
   - a `CLAIM` lease of 10 min renewed every 3, timed on the receiver's clock;
   - `REEXPAND` locks;
   - the spillover rule;
   - the speed factor that excludes stolen work.
5. **Chaos tools:** kill points via an environment hook at every commit step, an injectable clock with jumps, simulated disk-full, and process-level partitions on one machine.
6. **Agent-exit criteria:**
   - a node joins mid-run in the multi-process test;
   - killing a node mid-run loses no committed work and the others don't restart;
   - an idle node steals without duplicate children;
   - a 2-and-2 split moves nothing;
   - a lone eviction is rejected;
   - the chaos suite passes;
   - CI green.

   **OA:** Linux machine join, and a power pull on a Mac.

### M8 · Depth and scale (branch `m8-depth-scale`)
1. Branch shares (50% of the parent's remaining pool) and starting pools proportional to open tickets; the leaf with end = "allowance spent".
2. Depth checkpoints every N levels past L9 (no attempt spent; `NEEDS_REVIEW` with reason depth checkpoint); remove every L9 guard.
3. Compaction of `ATTEMPT` and `CRITIQUE` records older than 30 days into checkpoint summaries, with the totals kept.
4. Cost and log-growth projections from measured data.
5. Observability in `/api/state` and the dashboard: lanes, claims, steals, review backlog, spend rate, disk growth per device, quality guard.
6. Parquet export stays out (DuckDB needs the owner): an OA question only.
7. **Agent-exit criteria:**
   - a single chain under a 50% share terminates;
   - a fake model that never says "atomic" still terminates;
   - a depth checkpoint spends nothing;
   - compaction keeps the totals;
   - a funnel-scenario campaign past L9 ends within its pools;
   - measured log growth within ±25% of the projection;
   - CI green.

### M9 · Hardening (branch `m9-hardening`)
1. **Security review:** `docs/SECURITY_REVIEW.md` with every row of the `DESIGN.md` threat table and its evidence (test name, code reference). Fix any gap found.
2. **Fuzzing:** libFuzzer targets (clang on Linux CI) for:
   - the batch parser;
   - the canonical-JSON reader;
   - the sync HTTP request parser;
   - the LLM response parser;
   - the CSV import.

   Run 10 minutes per target in a scheduled CI job. Every crash found becomes a regression test.
3. **Chaos:** the full suite across processes, with every injected fault.
4. **Documentation:** an operator guide (install per OS, invite, join, campaigns, review, budgets, recovery), troubleshooting, the complete CLI reference, and an upgrade guide.
5. **Windows:** a packaging or installer script, the firewall rule (Private profile only), and the service.
6. **Legacy:** open a PR that deletes `legacy/` and the legacy CI jobs. **Do not merge it.** Record it as an OA.
7. **Agent-exit criteria:** the security review complete with no open items (or each with an OA); fuzzers running with no unresolved crash; the chaos suite green; docs complete; CI green; the legacy-deletion PR open.

   **OAs:** Windows 10/11 joins, the 24 h mixed soak, merging the legacy PR, tagging `v1.0`.

---

## 12. Definition of done (the goal)

The goal is met when all of the following hold, each with evidence:

- [ ] G1–G9 resolved and recorded.
- [ ] M2–M9 each merged into `main` through a PR with all CI jobs green, and tagged `mN-rc`.
- [ ] `docs/agent/gates/M2.md` … `M9.md` written, each with agent-verified evidence and an owner-verification checklist.
- [ ] `TRACEABILITY.md`: every rule in `DESIGN.md` maps to at least one passing test, or to an owner-run live check.
- [ ] `OWNER_ACTIONS.md`: every live, hardware, money or approval step is a complete runbook.
- [ ] No Proposed decision record is unlisted in `OWNER_ACTIONS.md`.
- [ ] The legacy-deletion PR is open, not merged.
- [ ] Real API usage within the caps, and logged.
- [ ] The final report is written (§13).

---

## 13. Final report (`docs/agent/FINAL_REPORT.md`, and your last message)

1. **Outcome:** goal met / stopped at a hard stop / stopped because everything remaining is blocked.
2. **Milestones:** a table of milestone, merge commit, `mN-rc` tag, CI run link, test counts per job.
3. **Blockers:** each one with its write-up and what unblocks it.
4. **Owner actions, in recommended order**, with the estimated time and cost of each.
5. **Decision records added**, with their status.
6. **Real API usage** (calls, USD) and CI usage notes.
7. **Known limitations** and recommended next steps.

---

## 14. Quick reference: constants that must never drift

Gate 0.7 / 3 attempts / floor 0.5; temperatures 0, 0.3, 0.5; pre-check ≤ 400 tokens; light funnel from L8; ancestors 0.85 / 0.95; 3–100 children. Lease 10 min, renewed every 3; chunks of 50 attempts; branch share 50%; depth checkpoint every 5 levels past L9. SWIM 1 s / 500 ms / 2 indirect / DEAD at 5 min; beacon every 5 s; sync every 15 s; anti-entropy every 10 min; checkpoints every 6 h or 100k records, keep 3; segments 64 MB, zstd. Ports UDP 7946, TCP 7947, API 127.0.0.1:8080. Ids `slot × 2^40 + counter`, slot = top 20 bits; HLC `(ms << 16) | counter` as exact int64. Breaker 20 consecutive (401/402: 3); limiter window 30 s, halve on 429/503 or > 5% timeouts, +4 at ≤ 1.5× the probe baseline, probe hourly. Invitations 24 h, single use; request signatures ±60 s; auto-retire after 30 days. Profile: desktop weight 1.0, start 16, ceiling 2,000; laptop weight 0.25, start 4, ceiling 32, on-battery cap 8.

If code and this list disagree, check `DESIGN.md` and the decision records. If those disagree with this list, they win; note the discrepancy in `WORKLOG.md`.

---

## 15. Kickoff message (owner → agent)

> Your goal is defined in `AUTONOMOUS_GOAL.md`, which I'm giving you now. As your first commit on `m2-deepseek-client`, add it to the repo as `docs/agent/AUTONOMOUS_GOAL.md`. Read it completely, then `docs/DESIGN.md`, `docs/REBUILD_PLAN.md`, `docs/decisions/`, `docs/agent/IMPLEMENTATION_BRIEF.md` §5–§9, `docs/agent/PROGRESS.md` and the latest gate report.
> Owner settings are in §0. Work autonomously toward the goal in §12, following its rules. Don't ask me for approval. Stop only on the conditions in §1, and record everything that needs me in `docs/agent/OWNER_ACTIONS.md`.
> Start with the bootstrap and G1–G9 (§3.2, §11).
