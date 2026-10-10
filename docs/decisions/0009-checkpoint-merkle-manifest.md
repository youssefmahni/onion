# 0009 — Checkpoint Merkle leaf encoding and manifest format

## Context

M4 checkpoints carry a Merkle root over `nodes` sorted by path so a joiner can verify a snapshot. DESIGN.md does not specify the leaf encoding or the manifest file format.

## Decision

- **Leaf encoding (Accepted).** For each node, the leaf is BLAKE2b-256 of the canonical JSON (the record canonicalization of decision 0003) of `{"code","path","layer","title","hlc","tombstone"}`. Leaves are ordered by `path` (ascending, byte order). Internal nodes hash the concatenation of their children's digests; an odd tail is promoted.
- **Manifest (Accepted).** Each checkpoint writes `checkpoint-<hlc>.db` (the `VACUUM INTO` snapshot) and `checkpoint-<hlc>.manifest`, a JSON object: `{hlc, snapshot_hash (BLAKE2b-256 of the snapshot file), merkle_root, vector {origin: last_seq}, signature, cosignature}`. `signature` is the creating device's Ed25519 signature over the canonical JSON of the manifest with `signature` and `cosignature` set to empty strings; `cosignature` is a second device's signature over the same bytes (empty until M6).
- **Retention (Accepted).** The last 3 checkpoints are kept; older pairs are deleted after a new one is written.

## Consequences

A joiner recomputes the Merkle root from the snapshot's `nodes` table and compares it with the manifest, then verifies the signatures, before trusting the snapshot.
