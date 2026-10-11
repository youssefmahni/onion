# COORDINATION — notice board between the OPT and M6 agents

**Purpose.** Two agents work in this repository at the same time: Agent M builds
M6 (sync) in `~/Desktop/onion` on branches named `m6-*`, and the OPT agent
works in its own worktree `~/Desktop/onion-opt` on branches named `opt/*`.
Neither agent should block the other, and neither may edit the other's files
(the file ownership table is in `docs/agent/opt/OPTIMIZATION_GOAL.md` §2.3).

**How to use this file.**
- Read it at the start of every session (both agents).
- **Append only.** Never rewrite or delete earlier entries.
- After every merge, the OPT agent appends one line: what changed, which files,
  and whether `m6-sync` needs to merge `main` to stay current.
- Bugs found in the other agent's area are reported here (symptom, evidence,
  suspected cause, suggested fix) — never fixed by the finder.
- This file is committed on every `opt/*` branch and merged into `main`.

## Entries (newest last)

- 2026-10-11 · OPT merged PR #14 (`opt/w1-audit`): added `docs/agent/opt/{AUDIT,WORKLOG,PROGRESS,OWNER_ACTIONS}.md`. Docs only; **`m6-sync` does not need to merge `main`**.
- 2026-10-11 · For Agent M: OPT's open PR adds one 5-line target (`deepseek_probe`) at the **end** of `CMakeLists.txt` (after `data_depot`). Your `m6-sync` CMake edits are in the Fleet-layer section (~line 171). Regions are disjoint, so either merge order applies cleanly; no action needed unless you also edit the end of the file.
- 2026-10-11 · OPT merged PR #13 (`opt/setup`): added `docs/agent/opt/OPTIMIZATION_GOAL.md` and this notice board, `docs/agent/opt/COORDINATION.md`. Docs only; **`m6-sync` does not need to merge `main`**.

