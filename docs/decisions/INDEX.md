# Decision registry

This file is the **number registry**. Claim the next free number here (in the
same commit that adds the record, or earlier) **before** writing a record.
Never reuse a number. Keep the Status column in step with the record's own
`Status:` line.

Status vocabulary: **Accepted**, **Proposed**, **Superseded** (by NNNN).
`Accepted*` means the file has no `Status:` line of its own; the status is
inferred from the milestone gate or owner statement that shipped it (see the
Note column). Last audited 2026-10-14 against `origin/main` @ `cb0f3ab`
(REVIEW.md, finding F-DOC-1).

| No. | Title | Status | Superseded by | Note |
|---|---|---|---|---|
| 0000 | Owner clarifications before M1 | Accepted* | — | owner message |
| 0001 | Repository layout and dependency direction | Accepted* | — | M1 gate |
| 0002 | TLS backends by platform | Accepted* | — | M1 gate |
| 0003 | Record-number canonicalization | Accepted* | — | M3/M4 gates |
| 0004 | DeepSeek model and price configuration | Accepted* | — | M2 gate |
| 0005 | Request-limiter probe baseline | Accepted* | — (limiter behaviour extended by 0011) | M2 gate |
| 0006 | LLM error outcomes | Accepted* | — | M2 gate |
| 0007 | Separate connect timeout from total timeout | Accepted | — | |
| 0008 | Campaign file format, stop rule, review-children storage | Accepted* | stop-rule clauses to be superseded by 0024 (B3) | M3 gate; amended by 0017 |
| 0009 | Checkpoint Merkle leaf encoding and manifest | Accepted* | — | M4 gate |
| 0010 | Campaign liveness: exhausted vs finished | Accepted | — | M6 gate |
| 0011 | Limiter v2: goodput control and client hygiene | Accepted | — | |
| 0012 | Profile rules v2: ceilings from real measurements | **Proposed** | — | owner decision DR_0012 (stays Proposed) |
| 0013 | SWIM wire defaults and the LAN beacon group | Accepted* | — | file heading still says "(Proposed)" but M7 shipped it (finding F-DOC-2) |
| 0014 | Budget safety: the overshoot bound that actually holds | Accepted | — | owner approved 2026-10-11; amendment 2 (majority rounds) 2026-10-13 |
| 0015 | HTTP version pinned to HTTP/2 with HTTP/1.1 fallback | Accepted | — | |
| 0016 | Campaign plan projection model | Accepted | — | to be regenerated from B4 measurements |
| 0017 | Funnel defaults v2 | Accepted (in principle) | gate defaults deferred to 0024 | owner 2026-10-13 |
| 0018 | Sync request replay protection: fresh 128-bit nonce | Accepted* | — | M7 owner review |
| 0019 | macOS key storage: file-first, keychain fallback | Accepted* | — | |
| 0020 | Critic rubric calibration | **Proposed** | to be superseded by 0024 (B3, GATE_CHANGE approved) | on main |
| 0021 | Batched atomic verification | Accepted | — | title still says "(Proposed)" (F-DOC-2) |
| 0022 | M8 checkpoint compaction, measured growth, operator state | Accepted | — | on `m8-depth-scale`, not yet on main |
| 0023 | Branch shares: the pool ledger (50% share) | Accepted | — | **renumbered from a second "0020"**; file `0020-branch-share-pool-ledger.md` on `m8-depth-scale` is renamed in B5 and every "decision 0020" reference in code and docs of that branch is updated |
| 0024 | Quality gate and funnel fixes (B3) | *claimed, not yet written* | supersedes 0020; amends 0008 stop rule and 0017 gate defaults | claimed 2026-10-14 by the takeover agent |
| 0025 | Branch-share allowances apply to engine-created expansion only (imported tickets never leaf as "allowance spent"); the no-progress churn guard | *claimed 2026-10-14 (Mac Pro live-test finding)* | amends 0023 (the pool ledger) |
| 0026 | Dashboard frontend stack (Preact+htm+uPlot, vendored, no build), 127.0.0.1-only access with SSH tunnel, and the `--allow-remote-dashboard` bind gate | **Proposed** | covers DASHBOARD_PLAN.md D-1, D-2, §3.3 | claimed 2026-10-15 (dashboard D0) |
| 0027 | Import safety and domain profiles (RE&CT readiness) | Accepted (owner directive) | amends 0025 (source field) | on b3-gate-funnel |
| 0028 | Remove audit sampling from the engine | Accepted (owner directive) | supersedes 0024's audit-sampling clauses; renumbered from 0026 after the dashboard record claimed it first | on b3-gate-funnel |
| 0029 | Domain-profile level semantics and ordered children | Accepted (owner directive) | amends 0027 (named-tree code collisions are per parent; profile ranges own the fallback floor) | on `b4-react-profile-support`; claimed 2026-10-15 |

Next free number: **0030**.

## Rules

1. One number, one record. A second file with the same number is a defect.
2. A record that changes the behaviour of an earlier one says so in its first
   lines (`Supersedes: NNNN`) and the older record gets `Superseded by: NNNN`.
3. `DESIGN.md` must match every Accepted record; a disagreement is reported
   and fixed through a record, never silently in code.
