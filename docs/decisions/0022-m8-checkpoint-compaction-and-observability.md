# 0022 — M8 checkpoint compaction, measured growth, and operator state

Status: **Accepted**

## Context

M8 removes the fixed-depth ceiling without allowing the audit history or the
operator view to become unbounded. The plan requires 30-day ATTEMPT/CRITIQUE
compaction with totals retained, a measured log-growth forecast, and a single
dashboard state view for fleet work and storage.

## Decision

1. **Compaction boundary.** A checkpoint first copies `onion.db` with
   `VACUUM INTO`, then replaces ATTEMPT and CRITIQUE rows older than 30 days
   in that *checkpoint copy* with one `checkpoint_summaries` row per path.
   Each row retains attempt/critique/pass counts, input/cached/output tokens,
   first/last HLC, and the exact cost total in USD nanounits. The source
   database and signed operation segments remain replayable audit history.
   A clone includes its summaries in lifetime-spend reporting.
2. **Checkpoint cadence.** The writer creates a checkpoint after 100,000
   applied records or six hours, whichever is first, and retains three. A
   failed write does not clear the due counter, so it retries on the next
   writer turn.
3. **Growth measurement.** `measure_params` measures active and sealed oplog
   bytes divided by settled nodes. The campaign plan multiplies that number
   by projected expanded nodes (not retries); its acceptance helper accepts
   an observed result only within ±25%.
4. **Operator state.** `/api/state` supplies lane ticket counts, active
   receiver-timed claims, locally-owned active steals, review backlog, recent
   hourly spend, oplog bytes by origin, total data bytes, and the existing
   quality-guard score/failure signal. The local dashboard renders those
   fields without interpolating untrusted values as HTML.
5. **Depth configuration.** Depth checkpoints are opt-in per campaign to
   preserve campaigns recorded before M8: both fields absent/zero mean off.
   The documented five-level policy is `depth_checkpoint_from: 14` and
   `depth_checkpoint_every: 5`, producing L14, L19, … pauses with no attempt
   spent. Accept or retry only releases such a ticket; it does not settle or
   re-expand it.

## Consequences

- A compact checkpoint can seed a new node without losing audit totals, while
  immutable logs remain the authority for complete replay.
- The projection is a measurable storage budget rather than a fixed record
  size guess.
- DuckDB/Parquet is intentionally not added; it remains owner action OA-17.
