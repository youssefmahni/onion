# 0015 — HTTP version pinned to HTTP/2 with HTTP/1.1 fallback

Status: **Accepted** (implementation detail; DESIGN.md already specifies the
behaviour, this record makes the pin explicit and airtight).

## Context

The client sets `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2TLS` (HTTP/2 over
TLS, HTTP/1.1 fallback), which is DESIGN.md's behaviour. During the W3 real
smoke the negotiated version was reported as `CURLINFO_HTTP_VERSION = 3`,
which was initially misread as HTTP/3; **3 is the enum value for HTTP/2.0**
(HTTP/3 would be 30). The transport has always been HTTP/2.

Curl 8.x can silently upgrade a transfer to HTTP/3 through the Alt-Svc cache
or HTTPS DNS records. Neither is wanted here: the limiter and the failure
classification are tuned against a known transport, and a silent protocol
change behind their back is exactly the kind of surprise this track exists to
prevent.

## Decision

- The engine client pins HTTP/2 with HTTP/1.1 fallback: `CURL_HTTP_VERSION_2TLS`
  (already set) plus `CURLOPT_ALTSVC ""`, which keeps the Alt-Svc cache off
  so no silent HTTP/3 upgrade can occur. HTTPS-RR lookups are not enabled.
- The blocking `http_get` (`/models` check) applies the same pin.
- Windows negotiates HTTP/1.1 only (decision 0002, accepted limitation); the
  pin changes nothing there.
- HTTP/3 adoption, if ever, requires evidence first: a real comparison run
  (at most 300 tiny calls, at most 300 in flight) showing HTTP/3 at least as
  fast and error-free as HTTP/2 on the same key, recorded as a follow-up
  decision. No such run is needed now because HTTP/3 was never in use.

## Consequences

- The transport is fixed and observable: `negotiated_http_version()` reports
  HTTP/2 (enum 3) against DeepSeek, HTTP/1.1 against the fake server (enum 2).
- No behaviour change for the fleet: the pin matches the observed transport.
