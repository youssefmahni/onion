#!/bin/sh
# Weekly genesis-import dry run against the corpus copy (docs/CUTOVER.md §1).
# Run from anywhere; prints VERIFIED / VERIFICATION FAILED and the counts.
set -eu

REPO="${REPO:-$HOME/Desktop/onion}"
CORPUS="${CORPUS:-$HOME/onion-data}"
DRYRUN="${DRYRUN:-$HOME/.local/share/onion-dryrun}"
BIN="$REPO/build/linux-debug/data_depot"

if [ ! -x "$BIN" ]; then
  echo "build first: cmake --preset linux-debug && cmake --build --preset linux-debug" >&2
  exit 2
fi
if [ ! -f "$CORPUS/Onion - level_00.csv" ]; then
  echo "corpus copy missing at $CORPUS (see OWNER_ACTIONS.md OA-2)" >&2
  exit 2
fi

mkdir -p "$DRYRUN"

if [ -f "$CORPUS/duplicates-resolution.tsv" ]; then
  RESOLUTION="$CORPUS/duplicates-resolution.tsv"
else
  "$BIN" genesis-import --root "$CORPUS" --data-dir "$DRYRUN" \
    --propose "$CORPUS/duplicates-proposal.tsv" >/dev/null
  RESOLUTION="$CORPUS/duplicates-proposal.tsv"
  echo "note: no approved resolution yet; using the fresh proposal"
fi

"$BIN" genesis-import --root "$CORPUS" --data-dir "$DRYRUN" \
  --resolution "$RESOLUTION"
