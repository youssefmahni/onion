# TLS backends by platform

## Context

M1 pins libcurl for the future DeepSeek client. The fleet must build on macOS 12, Linux, and Windows without inventing a different certificate policy per machine. curl 8.15.0 (July 2025) removed the Secure Transport backend, so it cannot be the macOS choice for the pinned curl line.

## Decision

- macOS: link the libcurl shipped by the macOS SDK. It uses Apple's supported transport and trust-store integration; Onion does not build or vendor curl for macOS.
- Linux: link the distribution libcurl built with OpenSSL. The supported package is `libcurl4-openssl-dev`; Onion does not build or vendor curl for Linux.
- Windows: build the pinned vendored libcurl release with Schannel.
- M2's CMake configuration must require libcurl 7.66.0 or later. That is the first release containing `curl_multi_poll`, which the planned multi-threaded event loop uses, and is newer than the 7.30.0 introduction of `CURLMOPT_MAX_TOTAL_CONNECTIONS` used to bound connections.
- Certificate verification is mandatory. The LLM and sync HTTP code must not set `CURLOPT_SSL_VERIFYPEER` or `CURLOPT_SSL_VERIFYHOST` to a disabling value, and tests may only trust an explicitly supplied test CA.

## Consequences

macOS and Windows use their native certificate stores and TLS maintenance paths. Linux uses the distribution-supported OpenSSL package. M2 owns the concrete libcurl build integration and verifies HTTPS requests against the fake server; no TLS transport is implemented in M1. The dependency manifest records that the pinned curl archive is Windows-only; system curl versions are validated by CMake when the LLM client is enabled.

## Update 2026-10-10 (M2, G5/G6)

The Windows pin moves from curl 8.11.1 (December 2024) to **curl 8.22.0**
(released 2026-09-02; 38 days old at the update). SHA-256
`f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7`, verified
against both the curl.se and GitHub copies of `curl-8.22.0.tar.xz`. Schannel
remains the TLS backend (`CURL_USE_SCHANNEL`), so certificate validation still
uses the Windows trust store.

The vendored Windows curl is built without nghttp2, so Windows negotiates
HTTP/1.1 only. This is an accepted limitation (owner decision, G6): the client
requests HTTP/2 via `CURL_HTTP_VERSION_2TLS` and falls back to HTTP/1.1
automatically. Recorded here so the limitation is never mistaken for a defect.
