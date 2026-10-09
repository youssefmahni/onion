# TLS backends by platform

## Context

M1 pins libcurl for the future DeepSeek client. The fleet must build on macOS 12, Linux, and Windows without inventing a different certificate policy per machine.

## Decision

- macOS: build libcurl with Secure Transport.
- Windows: build libcurl with Schannel.
- Linux: build libcurl with OpenSSL supplied by the supported distribution.

## Consequences

macOS and Windows use their native certificate stores and TLS maintenance paths. Linux uses the distribution-supported OpenSSL package. M2 owns the concrete libcurl build integration and verifies HTTPS requests against the fake server; no TLS transport is implemented in M1.
