# Onion Knowledge Engine: project rules for Claude Code

## What this project is

A C++17 engine that grows a deep knowledge tree with an LLM funnel (DeepSeek). It runs as one binary per machine on a masterless fleet (macOS 12+, Linux, Windows 10/11) that syncs through signed, hash-chained, append-only logs.

- `legacy/` holds the original engine. It is frozen and is deleted only in M9, with the owner's approval.

## Sources of truth (read when relevant; don't guess)

1. **The owner's messages.**
2. **`docs/DESIGN.md`**, the architecture, and **`docs/decisions/`**: decision records (numbered; check status Accepted/Proposed/Superseded).
3. **`docs/REBUILD_PLAN.md`**: milestones and exit gates.
4. **`docs/agent/`**:
   - `PROGRESS.md`, `WORKLOG.md`, `OWNER_ACTIONS.md`, `TRACEABILITY.md`;
   - gate reports in `gates/`;
   - `AUTONOMOUS_GOAL.md` (rules §6–§9, plan §11).
5. **`docs/agent/opt/`**: the finished optimization track (`FINAL_REPORT.md`, `COST_MODEL.md`, `FUNNEL_OPT.md`, `COORDINATION.md`).

If code and `DESIGN.md` disagree, report it. Change behavior only through a decision record.

## Build and test

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug -j"$(nproc)"
ctest --preset linux-debug                  # full suite
cmake --preset linux-release && cmake --build --preset linux-release   # chaos hooks OFF
export TMPDIR="$PWD/.build-tmp"; mkdir -p "$TMPDIR"   # /tmp is small: never build or test into /tmp
export POCL_CACHE_DIR="$PWD/.build-tmp/pocl"          # GPU tests need a writable cache
```

- CI runs 8 jobs (`.github/workflows/ci.yml`): Linux, ASan+UBSan, TSan, macOS arm64, macOS x86_64/12.0 build, Windows, legacy Linux, legacy macOS. Plus `opt-nightly.yml`.
- Use `gh` for PRs and runs. Poll briefly; don't block on long watches.

## Non-negotiable rules

**Git**
- One branch per task or milestone, always from fresh `origin/main`.
- Merge only through a PR with a merge commit (`gh pr merge --merge`), after all CI jobs pass.
- Never commit to `main`, never force-push, never rewrite pushed history.
- Tags: `mN-rc` after a milestone merges; only the owner creates final tags.
- Stage explicit paths only (never `git add -A` or `git add .`). Run `git status` before every commit.

**Secrets**
- Never read, print, log or commit API keys, private keys, or anything under `.aws/`, `.env` or `*.pem`.
- Before every commit:
  ```bash
  git diff --cached | grep -nEi 'sk-[a-z0-9_-]{16,}|BEGIN [A-Z ]*PRIVATE KEY|(api[_-]?key|secret|token)[^\n]{0,8}[:=][^\n]{0,4}[A-Za-z0-9_-]{20,}' && echo STOP || true
  ```

**Money**
- Real DeepSeek calls only within the caps in the current mission prompt.
- The key comes from `DEEPSEEK_API_KEY`, else per command:
  ```bash
  DEEPSEEK_API_KEY="$(cat ~/.config/onion/deepseek_key)" <command>
  ```
  Never echo it.
- Log every real batch (date, purpose, calls, tokens, cost) in `docs/agent/PROGRESS.md`.

**Data**
- `~/Desktop/PlanningProcess` is the live corpus: read-only. Work only on a copy (`~/onion-opt-data`).
- Never touch files outside the repo, its temp dirs and the corpus copy.

**Integrity**
- Never delete, skip, weaken or "retry until pass" a test to get green.
- Never claim a result without the command output, CI link or commit hash that proves it.
- A failing test is evidence about the product first.

**Anti-loop**
- 3 failed attempts on one problem → change strategy (smallest reproduction, instrumentation, docs).
- 6 attempts or 4 hours → write it up as BLOCKED in `WORKLOG.md` and move on to independent work.
- The same CI failure on 3 consecutive pushes counts as a loop.

**Portability**
- OS calls only in `src/platform/`.
- LF and UTF-8; no floats in records (decision 0003).
- Test hooks only in test builds (`ONION_CHAOS_HOOKS`).
- Network tests bind port 0. Never use fixed ports.

**Decision records**
- Claim the next number in `docs/decisions/INDEX.md` (create it if missing) **before** writing the record.
- Never reuse a number.

## Memory between sessions

The repo is the memory:
- After every task, append to `docs/agent/WORKLOG.md`: result, evidence, a **Next step:** line.
- Keep `docs/agent/PROGRESS.md` current. It is the single status source.
- At session start, read the latest `WORKLOG.md` entries and continue from the last **Next step:** line.
