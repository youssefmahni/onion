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
