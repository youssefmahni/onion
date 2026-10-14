# Campaign file format, stop rule, and review-children storage (M3)

## Context

M3 introduces the one-machine engine. Three things the spec leaves open had to be fixed to build it: the campaign JSON file format, the exact semantics of the campaign stop rule, and where the children of a `NEEDS_REVIEW` node live.

## Decisions

1. **Campaign file format (Accepted; amended by decision 0017).** `campaign start <file>` reads a JSON object with the keys: `id` (string, required), `scope` (array of path prefixes, default empty = whole tree), `loop_budget` (positive integer attempts, default 10,000), `mode` (`breadth-first` or `depth-first`, default `depth-first`), `gate` (0.7), `max_attempts` (3), `floor` (0.5), `chunk` (RESERVE chunk size, default 50), `light_funnel_from` (default **6**), `structure` (bool, default false), `model` (default `deepseek-flash`), `critic_model` (default: the model), `auto_accept` (bool, default false). Unknown keys are ignored. The same JSON (canonicalized through the record machinery) becomes the `CAMPAIGN` record's `spec` payload.

2. **Stop-rule semantics (Accepted).** "last 50 settled nodes ≥ 90% atomic and average score ≥ 0.8" counts, over the last 50 settled nodes in settle order, a node as *atomic* when its terminal `SETTLE` recorded at least one atomic child (`settles.atomic ≥ 1`). The window needs 50 settled nodes; the average is over the `SETTLE.score` of those nodes. Leaves contribute `atomic = 0`.

3. **Review children storage (Proposed).** A `NEEDS_REVIEW` terminal batch carries the accepted-attempt children (`ADD_NODE`) and `CRITIQUE` before the `NEEDS_REVIEW` record, and the `NEEDS_REVIEW` payload additionally holds `attempts` and `score` (the op-table payload `path, reason` is extended, never shrunk). The children exist in the store but the scheduler never makes tickets under an open review, so they stay invisible until the review is decided. `review accept` settles the node (`REVIEW` + `SETTLE`, no spending); `review retry` records `REVIEW` + `REEXPAND` and re-queues the node with the note injected (existing children are filtered by the canonical-title dedup); `review reject` tombstones the node and its subtree. This preserves "spend nothing more on this node" while keeping a restart able to accept a review without re-running the funnel.

## Consequences

- The stop rule is a pure, tested function (`sched::stop_rule_met`).
- Review decisions survive restarts: children, attempts and score are all in the log.
- If the owner prefers review acceptance to re-run the node instead of keeping stored children, only the fold for `REVIEW`/`NEEDS_REVIEW` changes (see OWNER_ACTIONS.md OA-8).
