# 0019 — macOS key storage: file-first, keychain as migration fallback

## Context

OA-13's layer 2: `KeyStore::get` on macOS used the system keychain first
and failed or misread when the login keychain was locked (headless CI,
screen-locked machines), yielding 401s that looked like network failures.
The owner asked for the decision record.

## Decision

1. **File-first.** The device key lives in a 0600-permissioned file under
   `<data-dir>/keys/`. `KeyStore::get` reads that file first on every
   platform. The file is the canonical store.
2. **Keychain as one-way migration fallback, fail-closed.** Only when the
   file is absent does the implementation consult the macOS keychain; a
   hit is migrated to the file and the read succeeds. Any keychain error
   or ambiguity does NOT fall back to generating a new key — it fails
   closed, because silently minting a fresh device identity would fork
   the log.
3. **No platform special cases elsewhere.** Everything above
   `KeyStore` works with the seed hex only; no other code path touches
   the keychain.

## Consequences

- Headless and locked-keychain macOS machines behave identically to
  Linux: the 0600 file is read, the keychain is never a dependency.
- The existing keychain-only deployments migrate transparently on first
  read after upgrade.
