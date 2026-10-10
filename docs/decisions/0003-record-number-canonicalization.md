# 0003 — Record-number canonicalization

## Context

Record HLCs are `(physical_ms << 16) | logical_counter`. They are already above
2^53, so RFC 8785's ECMAScript-number rule cannot preserve every signed 64-bit
record value exactly.

## Options

1. Use RFC 8785 number serialization unchanged.
2. Encode large values as JSON strings.
3. Define an integer-only record-number rule.

## Choice

Records contain only signed 64-bit integers. Each is written as exact base-10
decimal digits; floating-point numbers are forbidden. Object ordering, string
escaping, and all other canonical JSON behavior follow RFC 8785.

## Consequences

This deliberately differs from RFC 8785 above 2^53. It preserves HLCs and
other signed 64-bit record values byte-for-byte on every supported platform.
Implementations reject record floats and out-of-range integers.
