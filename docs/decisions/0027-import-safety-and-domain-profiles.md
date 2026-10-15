# Decision 0027 — Import safety and domain profiles (RE&CT readiness)

Status: Accepted (owner directive 2026-10-15)

## Context

A second knowledge tree is planned on the RE&CT incident-response
framework, and the import path today has sharp edges the live fleet
already paid for (a forked hash chain from a second writer on a live
data dir). The funnel's planning-domain language is hard-coded, and
imported-ticket semantics key on an HLC comparison.

## Decision

### PR 1 — Import safety (its own PR)

1. **Exclusive data-dir lock.** `node run` holds an exclusive lock
   (`<data-dir>/node.lock`, OS advisory lock: `flock`/`LockFileEx`) for
   its whole lifetime. Every import or writer command acquires the same
   lock before touching the data dir and refuses with a clear message
   when it is held ("data dir is in use by a running node"). The lock
   dies with the process, so crashes never strand it.
2. **`source` field.** Imported node records carry an explicit
   `source: "import"` (new `nodes.source` column). Decision 0025's
   imported-ticket exemption keys on that field instead of the
   node-hlc vs campaign-start-hlc comparison.
3. **Collisions.** Import collisions (duplicate paths with different
   content, and code collisions within one tree) are reported per row to
   a file, and the import FAILS if any occur — unless `--allow-skip` is
   given, which skips them exactly like today's INSERT OR IGNORE.
4. **Checkpoint on success.** A successful import writes a checkpoint
   itself (the writer-thread path), so a joiner's clone includes the
   new tree without waiting for the 6 h / 100 k cadence.
5. Tests for each of the above.

### PR 2 — Domain profiles

1. One signed profile record per tree, distributed with the log; the
   campaign record carries the profile's hash. A device that does not
   hold that exact profile does not run the campaign.
2. The deterministic style checks in code (Title Case, word limits,
   punctuation) move into the profile — not only the prompt text.
3. `expandable_layers` is named `structure_layers` (the layers that need
   the structure permission and, without it, go to review).
4. A `tree` field on nodes: campaigns, exports and the dashboard filter
   by tree; node codes may repeat across different trees.
5. The planning profile is embedded and stays the default, byte-identical
   in behaviour — proven by the existing golden and engine tests passing
   unchanged.

### PR 3 — RE&CT (after 1 and 2)

A converter from RE&CT YAML (RA codes as node codes, `tree: react`), a
test import on a copy, and a 12-action pilot campaign whose results all
go to review.

## Consequences

- The live-node second-writer fork becomes impossible through the CLI.
- Imported tickets are identified by an explicit field, not a clock
  heuristic.
- The planning tree's current behaviour is frozen as the default
  profile; every other domain is a profile away.
