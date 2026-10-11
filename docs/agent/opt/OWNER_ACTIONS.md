# OPT track — OWNER_ACTIONS (append-only; newest first)

Format from OPTIMIZATION_GOAL.md §10: each entry is executable without asking the agent anything.

---

### OA-1 · Relay the M6 coordination note · W1 · status: open

Why: OPTIMIZATION_GOAL.md §13 contains a short coordination note for the agent
building M6 (PR #12, branch `m6-sync`). It tells that agent that a second agent
works in `~/Desktop/onion-opt` on `opt/*` branches, which files never to touch,
and to read `docs/agent/opt/COORDINATION.md` at the start of each session.

Prerequisites: the session in which the M6 agent runs.

Steps:
1. Paste the note from §13 of `docs/agent/opt/opt/OPTIMIZATION_GOAL.md`
   (in the repo: `docs/agent/opt/OPTIMIZATION_GOAL.md`, section "13. Coordination
   note for Agent M") into the M6 agent's session, unchanged.
2. Confirm to the OPT agent that it was delivered (or paste the note into
   `docs/agent/opt/COORDINATION.md` yourself as an entry).

Pass criteria: the M6 agent acknowledges the four numbered points.

If it fails: record the M6 agent's response in `docs/agent/opt/COORDINATION.md`.

Cost / duration: no API calls; under 5 minutes.
