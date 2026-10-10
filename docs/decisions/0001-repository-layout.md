# 0001 — Repository layout and dependency direction

## Context

The rebuild must coexist with the frozen `legacy/` system until M9, while
keeping OS code isolated and dependencies directional.

## Options

1. Extend `legacy/` in place.
2. Build the replacement in a separate tree with explicit layer targets.

## Choice

The replacement lives in `src/`, `tests/`, `tools/`, `third_party/`, and
`docs/`; `legacy/` remains independent until approved deletion in M9. CMake
targets enforce the layers Base, Data, Fleet, Engine, and Interface.

## Consequences

New modules cannot link against legacy code except copied pure-core sources.
Platform-specific calls remain in `src/platform/`, and tests can exercise the
replacement without touching the live corpus.
