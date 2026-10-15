# Decision 0029 — Domain-profile level semantics and ordered children

Status: Accepted (owner directive 2026-10-15)

Amends: 0027

## Context

Decision 0027 made the planning rules profile-owned and introduced a second
RE&CT tree.  The first profile format could express only one set of title
checks and structure layers.  It could not bind level-specific rules, child
counts, prompt text, or the execution order of procedural children to a
signed profile record.  The owner's RE&CT draft
(`docs/react.domain-profile.json`) needs all three, and the planning profile
must stay the default, byte-identical in behaviour.

## Decision

1. **Strict load-time validation.**  A profile is validated by one strict
   loader (`domain::parse_profile`) wherever it is read: `profile add`, the
   `PROFILE` fold on every device, the converter, and the engine.  It rejects
   unknown fields and wrong types; a missing stage prompt; a placeholder a
   stage is never given, or a required one that is missing (`{aspect}` in the
   worker, `{candidates}` in the trimmer, and so on); a `placeholders` entry
   the engine never fills; a child range or `ordered_children` on a level
   whose children are imported; a `source` that disagrees with
   `imported_layers`; style rules for an imported level; any layer past
   `levels.max`; a near-miss placeholder spelling (`{Title}`, `{ title }`,
   `{made-up}`); the reserved tree name `planning`; and a document over
   256 KiB **in its signed form** (`profile add` also re-validates that form to
   the same hash, and checks the record folded, before it reports success).
   The content hash covers the whole validated document.  Records obey decision 0003, so critique-anchor
   scores are exact decimal strings (`"0.8"`), never JSON floats; the owner's
   draft was changed in exactly that one respect.
2. **Per-level contract.**  For a ticket at parent level *L* the profile
   supplies the child range, the child title rules, the child-level name and
   the child guidance for level *L + 1*.  A profile range overrides the
   campaign's generic target for that ticket, **including the
   exhausted-attempts floor** (`GateParams.fallback_min`): the floor is 3 for
   the planning default (decision 0024, unchanged) and the range minimum for a
   profile, so a legitimate two-step procedure is not leafed.  A level deeper
   than the last definition repeats the last one up to `levels.max`.
3. **`levels.max` is a depth limit.**  A ticket at level `levels.max` is closed
   as a leaf (`SETTLE end = "profile depth limit"`) without a model call.  The
   planning profile declares no levels and keeps growing without a cap.
4. **Title rules are enforced in code and rendered.**  Every mechanically
   checkable rule (word and character limits, capitalisation, questions and
   bangs, emoji, trailing period, code spans, allowed punctuation) is checked
   by the deterministic gate for the child level, and the same rules are
   rendered into `{title_rules}` (including filler words, trailing
   punctuation, code spans, and character limits counted in characters, not
   bytes).  `allow_code_span` is tri-state: unset checks nothing (the planning
   default, so a backticked planning title stays valid), `false` forbids code
   spans, `true` allows one paired span.  Free-text shapes such as "imperative
   verb phrase" are prompt text; they are not represented as a grammar parser.
5. **Ordered children.**  Children of a parent whose level sets
   `ordered_children` carry a zero-based `sibling_order` in their signed
   `ADD_NODE`.  The fold stores it (`nodes.sibling_order`, `-1` = legacy).
   `/api/tree` returns it and orders children by it, with pagination stable;
   the CSV export adds a `sibling_order` column **only when an exported node
   (within the exported layers and tree filter) has one** (`-1` marks an
   unordered row), and the development import reads the column **named
   `sibling_order` in a file's header**: a file without it (every planning CSV,
   whatever extra columns it has), an empty cell or `-1` means "no explicit
   order", so a planning CSV imports exactly as before and an export re-imports
   byte for byte; the importer never invents an order from row sequence.
   `export-csv --tree planning` exports the planning tree alone (the tree name
   `planning` is reserved), so a planning export stays in its original shape
   even when the same database holds an ordered named tree; `/api/tree` lists
   planning roots ahead of named-tree roots.  Scheduling, ticket order and the
   checkpoint Merkle leaves keep reading plain path order, so ordering never
   changes what the planning flow sees.
6. **Planning is untouched.**  The embedded planning profile keeps its old
   canonical JSON and hash.  Planning exports keep their original columns,
   planning listings keep `ORDER BY path`, `/api/tree` nodes carry no new
   field, the planning import keeps the tree-wide code-collision rule of
   decision 0027, and the exhausted-attempts floor stays 3.
7. **Import-code collisions in a named tree are per parent.**  RE&CT category
   codes repeat under every stage (`S1-C1`, `S3-C1`), so in a named tree a code
   is unique among the siblings of one parent (against the store and against
   the rows of the same import); a duplicate sibling code still fails the
   import.  This amends 0027's tree-wide rule for named trees only.
8. **`review_all`.**  A campaign may set `review_all: true`: every accepted
   child set is persisted as `NEEDS_REVIEW` at every expandable level,
   independent of the structure-layer permission.  The RE&CT pilot uses it.
   The field appears in the campaign record only when set.  The terminal
   `CAMPAIGN` stop record of a **domain campaign** (a named tree, a non-planning
   profile or `review_all`) now carries the full spec: it previously carried a
   seven-key subset and, because the fold replaces the stored spec, erased
   `tree`, `profile_hash`, `structure`, the light-funnel depth and the review
   flag when a campaign stopped.  A **planning campaign keeps its historical
   seven-key stop record**, so its records are byte-identical.
9. **The RE&CT converter** materialises the three imported layers from the
   response-action id (`RA` + stage digit + category digit + two digits):
   stage `S<d>` (layer 0), category `C<d>` (layer 1, path `S3-C1`), action
   `RA<id>` (layer 2, path `S3-C1-RA3101`).  Action titles come from the RE&CT
   bundle (`docs/react.json`, `--names`), falling back to a readable form of
   the YAML slug.  It copies the checked-in profile (`--profile`, default the
   source checkout's `docs/react.domain-profile.json`) only after the strict
   loader accepts it, so conversion is bound to the hash campaigns publish.
   The reduced profile PR3 checked in as `docs/react/react-profile.json` is
   removed: there is one RE&CT profile, `docs/react.domain-profile.json`.

10. **Informational fields.**  `levels.label_format`, `levels.import_csv_pattern`,
    `levels.import_codes` and `atomic.expected_from_level` are validated and
    covered by the profile hash, but the engine does not read them (the importer
    fixes its own file names, `Onion - level_NN.csv`).  They document the
    profile author's intent; giving any of them behaviour needs its own record.

## Rollout and compatibility

- The store schema version is now 2 (`nodes.sibling_order`; the migration adds
  each missing column independently, which also repairs a database that had
  `source` but not `tree`).  An older binary refuses a newer-schema database
  and a newer-schema checkpoint rather than misreading it.
- **An older binary does not fail loudly on this change; it diverges quietly.**
  Its profile loader is lenient: it accepts the RE&CT profile file but reads
  only the fields it knows, so it stores the profile under the *old* minimal
  hash, not the new one.  The one protection is that a RE&CT campaign pins the
  new hash: an older node does not hold it and writes the
  `profile ... not held` pause record, which pauses the campaign fleet-wide.
  Nothing else protects the fleet: an older node folds an upgraded peer's
  `ADD_NODE` and silently drops `sibling_order`; the checkpoint Merkle leaves
  exclude `sibling_order` (and `tree`, `source`), so its checkpoint still
  verifies, and a device that joins from it loses the order for good; and an
  older node ignores `review_all`.  **Upgrade every device before publishing
  the RE&CT profile or starting any ordered-children campaign.**  The 12-action
  pilot ran on a corpus copy for this reason.
- Open question for the owner, deliberately not changed here: putting
  `sibling_order` into the Merkle leaf (only for nodes that have one, so
  planning leaves stay identical) would turn the silent divergence above into a
  loud checkpoint mismatch.  It changes decision 0009's leaf encoding.
- `levels.max` and the per-level definitions are the profile author's to keep
  consistent; the loader checks coherence, not domain correctness.

## Consequences

- A RE&CT campaign is bound to its complete level and prompt contract, not
  merely a tree name and a few structure flags.
- Ordered incident-response steps survive commit, replay, export and the
  dashboard read.
- Planning behaviour is unchanged; the evidence is the golden and engine suites
  (including the 300-node offline campaign) passing, the original planning
  assertions restored in the store and state tests, and a planning import and
  export compared byte for byte against the pre-change binary.
- The 12-action pilot routes every generated result to review and needs a
  separately authorised DeepSeek budget for live calls.
