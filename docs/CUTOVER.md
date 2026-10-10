# Cutover from the legacy system to the new engine

The old system keeps producing until this cutover; the switch is one planned
event with a verified import, a parallel run and a rollback path that stays
open until M6 passes.

## 0. Prerequisites

- `docs/agent/OWNER_ACTIONS.md` OA-2 resolved: `~/onion-data` is a copy of the
  corpus, made from the confirmed `ORIGINAL_PROJECT` with `legacy/m0_backup.sh`.
- The admin key exists (`data_depot admin keygen --out <offline path>`), and
  its public key is recorded offline.
- A device key exists in the data directory (run `node run` once).

## 1. Weekly dry-run (already safe, run every week from now until the cutover)

```bash
data_depot genesis-import --root ~/onion-data --data-dir ~/.local/share/onion-dryrun \
  --propose ~/onion-data/duplicates-proposal.tsv
```

Inspect the proposal. If a resolution exists from an earlier week, reuse it:

```bash
data_depot genesis-import --root ~/onion-data --data-dir ~/.local/share/onion-dryrun \
  --resolution <approved-resolution.tsv>
```

A pass prints `VERIFIED` with per-layer counts, orphans=0 and the Merkle root.

## 2. Duplicate approval

The proposal lists every duplicated path with candidates and a recommended
keeper. The owner approves by copying the proposal to an approved resolution
file (editing the `kept` column where they disagree) and storing it with the
corpus copy. The import refuses to run without it.

## 3. Parallel run (M3→M4)

1. Build and run the new node against the fake funnel server for a smoke
   campaign; confirm the dashboard and review flows.
2. Run `genesis-import --resolution <approved>` into a NEW data directory and
   verify (`VERIFIED`, orphans=0).
3. Leave the legacy binary running on the corpus copy as the control.

## 4. Freeze

- Pause all legacy funnels. Let in-flight nodes finish. No new legacy runs.

## 5. Clean, import, verify

1. `genesis-import --root <copy> --data-dir <production-data-dir> --resolution <approved>`
   → `VERIFIED`, counts match the CSVs, orphans=0, Merkle root recorded.
2. `data_depot export-csv --data-dir <production-data-dir> --out <check-dir>` and
   diff the level files against the source CSVs.

## 6. Co-sign and switch

1. On a second device, clone the checkpoint and recompute the Merkle root
   (the manifest carries the co-signature field until M6; before M6 the
   single signature plus the owner's exported CSV check is the evidence).
2. Point the dashboard and any automation at the new `node run` process.
3. Start the first campaign.

## 7. Rollback

The legacy CSVs are untouched. To roll back: stop the new node, restart the
legacy funnel from the same corpus copy. Nothing in the new engine writes to
the corpus CSVs (they are read-only inputs).

## 8. The rollback window closes at M6

After the three-Mac soak (M6) passes, the rollback path is retired.
