# Owner clarifications before M1

## Context

The initial document review found ambiguities in test counts, attempt durability, platform staging, data-directory selection, golden fixtures, and hosted CI architecture coverage. The owner resolved them before M1 begins.

## Decisions

- The preserved legacy baseline is 98 tests: 90 core and 8 GPU. `legacy/` stays unchanged and continues to run all 98. M1 copies 67 pure-core tests and the 8 GPU tests into the new tree; the 23 integration-oriented core tests stay in `legacy/`. The `policy_target` tests remain with core until M2 replaces that function.
- Each finished attempt writes a durable, child-free `ATTEMPT` record. The state writer may group these records but flushes at least once per second. Terminal batches alone contain children, `CRITIQUE`, `ATOMIC`, and `SETTLE`, or `NEEDS_REVIEW`. `ATTEMPT.issues` is capped at 1,000 characters.
- Before M5, KeyStore uses a 0600 file in a 0700 directory on Linux and macOS, and DPAPI on Windows. M5 adds macOS Keychain and validates DPAPI in the Windows service account.
- Data-directory precedence is `--data-dir`, `ONION_DATA_DIR`, Linux `STATE_DIRECTORY`, then the documented OS user default. It is never selected from the user id.
- Golden tests use RFC 8032 section 7.1 Ed25519 vectors, BLAKE2 reference BLAKE2b-256 vectors, float-free RFC 8785 examples, and a project batch fixture signed from seed `00` through `1f`.
- Hosted CI tests native arm64 macOS and performs a separate Intel/macOS 12 compatibility build using `CMAKE_OSX_ARCHITECTURES=x86_64` and `CMAKE_OSX_DEPLOYMENT_TARGET=12.0`. A self-hosted Mac Pro runner is deferred. Linux installs POCL and runs GPU tests with a build-local cache; absent GPU runtimes report skips on macOS and Windows.

## Consequences

M1 can establish portable build coverage without claiming hosted macOS or Windows GPU execution. M3 preserves spend and retry history across a crash without exposing children before a gate decision. The first M1-specific repository-layout decision remains available as decision record `0001`.
