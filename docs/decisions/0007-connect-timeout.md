# 0007 — Separate connect timeout from total timeout

## Context

On Windows, connecting to a closed localhost port does not fail immediately:
the Windows TCP stack retries the connection attempt, and `WSAECONNREFUSED`
typically surfaces after about two seconds. With only a total timeout, a short
deadline hides the refusal as a timeout and a slow refusal is classified
`Timeout` instead of `Transport` (observed as 1,000 Timeouts on the hosted
Windows runner, zero connections). The owner's autonomous-goal brief §3.3
directs a separate connect timeout and total timeout so a slow refusal is
classified correctly in real use.

## Decision

`Client::Options` gains `connect_timeout_s` (default 10), set as
`CURLOPT_CONNECTTIMEOUT`, independent of the total `CURLOPT_TIMEOUT`
(`llm.timeout_s`). The developer CLI exposes `--connect-timeout`. The
closed-port regression keeps requiring `Transport` for all 1,000 refused
connections and uses a 10 s deadline. Status: **Accepted** (behavioural change
mandated by the owner's brief, not a spec constant).

## Consequences

A machine that cannot connect fails fast (at most `connect_timeout_s`) instead
of holding a worker for the full total timeout. With curl 8.22.0 on Windows
(which disables TCP SYN retransmissions for localhost), the refusal surfaces
immediately; the closed-port test passes in ~2.3 s there, and in ~0.3 s on
Linux.
