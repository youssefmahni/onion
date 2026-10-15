# 0026 — Dashboard frontend stack, remote access, and the bind gate

Status: **Proposed** (DASHBOARD_PLAN.md §0; agent-drafted, awaiting owner acceptance)

This record covers the owner decisions D-1, D-2 and the §3.3 bind change
from `docs/DASHBOARD_PLAN.md`. D-3…D-6 are recorded there for reference and
are not part of this record.

## Context

`data_depot` serves a control dashboard from its local HTTP API. Today that
API serves one `web/dashboard.html` file loaded from disk, and the node
binds `--host 127.0.0.1` by default but accepts `--host 0.0.0.0` (used for
the LAN fleet — the sync server advertises a reachable URL). The dashboard
plan replaces the single page with seven screens and adds authenticated
write endpoints, so the choices of frontend stack and network exposure
become load-bearing.

## Decision

### D-1 — Frontend stack: Preact + htm + uPlot, vendored, no build step

- `web/index.html` static shell; `web/app.js` router/store/stream client;
  `web/lib/` holds pinned vendored copies of Preact, htm and uPlot.
- No Node toolchain, no bundler: the shell loads plain scripts with
  `<script type="module">`-free tags; htm provides JSX-free templating.
- Vendored files are pinned with hashes in `third_party/VERSIONS.md`,
  like the existing third-party policy.
- Rejected alternative: a Vite build (adds Node to CI and a dist artifact
  to review). The static no-build layout keeps CI jobs unchanged and makes
  the embedded-bytes step (§3.1) trivial.

### D-2 — Remote access: 127.0.0.1 only; SSH tunnel for remote use

- The dashboard API binds loopback by default and is never exposed on the
  LAN by itself.
- Remote operators use `ssh -L 8080:127.0.0.1:8080 <host>`.
- Rejected alternative: LAN bind with TLS (certificates on every device,
  a CA story, and the fleet already uses signed custom transports for
  sync — the dashboard adds no value by re-solving that problem).

### Bind gate (§3.3)

- `--host` stays `127.0.0.1` by default.
- `--host 0.0.0.0` (or any non-loopback bind) now requires an explicit
  `--allow-remote-dashboard` flag on `node run`; without it, startup
  refuses the non-loopback host and prints the SSH-tunnel hint.
- With the flag, startup prints a warning: the dashboard is then reachable
  by anyone who can reach the port and knows/steals the session token.
- The sync server and SWIM bindings are unaffected: this gate is about the
  HTTP control surface only.

## Consequences

- Positive: the dashboard is unreachable from the network by default;
  scripts and the CLI (which already hold the device token) are unchanged.
- Cost: the Mac Pro fleet currently runs `node run --host <LAN IP>` to make
  the join response advertise a reachable sync URL (WORKLOG 2026-10-14).
  That setup must add `--allow-remote-dashboard` or move to the tunnel for
  dashboard access; the sync advertisement is independent of the HTTP bind
  and keeps working either way.
- D0 ships the gate and the flag; D1+ builds on the static shell.
