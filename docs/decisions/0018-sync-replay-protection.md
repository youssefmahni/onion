# 0018 — Sync request replay protection: fresh 128-bit nonce per attempt

## Context

OA-13 (macOS sync 401) had two layers. This record pins the replay layer's
exact contract, which the owner asked to be written down.

## Decision

1. **Nonce width and entropy.** Every signed sync request carries a nonce
   of at least 128 bits: two independent `std::uint64_t` draws from a
   `std::mt19937_64` generator seeded from `std::random_device`, hex
   encoded (16 bytes → 32 hex chars). Signed bytes are
   `device_id ‖ method ‖ path ‖ body-hash ‖ nonce ‖ timestamp`, so the
   nonce is unspoofable — the signature binds it.
2. **Fresh nonce per attempt, retries included.** Each retry of the same
   logical request signs NEW bytes with a NEW nonce (the retry is a new
   request, not a re-send). This is the layer-1 macOS fix: the httplib
   layer retries transports, and a retry of signed bytes with a replayed
   nonce was rejected as a replay. `signed_headers`/`signed_post_headers`
   generate the nonce inside the function, so every call gets a fresh one.
3. **Sliding window, keyed per device.** The server's `NonceCache`
   accepts a `(device_id, nonce)` pair once within the window; an exact
   replay within the window is rejected with 401 "replayed nonce".
   Entries age out so long-lived devices never fill the cache.
4. **Address recording inherits the same bar.** The push endpoint's
   `X-Onion-Sync-Url` recording runs only after `authenticate()` — the
   signature binds the `X-Onion-Device` identity — and only for a CURRENT
   member (row exists and is neither EVICTED nor LEFT, or listed in the
   static members). An unsigned or foreign-key push records nothing.

## Consequences

- `SyncTest.NonceCacheRejectsReplaysInTheWindow` and
  `SyncTest.RequestSignatureRoundTripAndRejections` pin the cache and the
  signed-bytes shape; `SyncTest.PushAddressRecordingRequiresACurrentMemberSignature`
  pins the unsigned/foreign-key/valid triple.
- The golden cross-OS test (`SyncGolden`) pins the exact signature bytes,
  so any drift in the canonicalization fails on every platform.
