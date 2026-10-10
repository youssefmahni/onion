# 0005 — Request-limiter probe baseline

## Context

Using the lowest median latency in a trailing hour lets sustained busy latency
replace every low-load sample. The limiter could then repeatedly grow while
the provider remains slow, contrary to the latency-rise hold rule.

## Choice

Only initial windows at the profile starting limit and one 30-second probe per
hour may update the latency baseline. A probe caps the window at
`max(starting limit, current limit / 4)`, records its median, then restores the
previous limit. If probe traffic has transport pressure, the restored limit is
halved and held like any other pressure event.

## Consequences

Busy windows cannot redefine normal provider latency. The initial baseline is
available without waiting an hour; later probes deliberately trade one small
window of throughput for a stable control signal.
