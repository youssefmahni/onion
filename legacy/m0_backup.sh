#!/usr/bin/env bash
# m0_backup.sh — M0: snapshot the corpus and every ledger before any change.
# Bash 3.2-safe (macOS). Usage: ./m0_backup.sh [project_dir] [dest_dir]
# Copy the resulting .tar.gz and .sha256 OFF this machine afterwards.
set -eu
src="${1:-.}"
dest="${2:-backups}"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$dest"
dest="$(cd "$dest" && pwd)"          # absolute, so it survives the cd below
out="$dest/onion-corpus-$stamp.tar.gz"
cd "$src"
files=""
for f in "Onion - level_"*.csv funnel_state.tsv funnel_critique.tsv atomic_items.tsv \
         node_llm_report.tsv expand_decisions.csv title_aliases.tsv llm_pending_expansion.txt; do
  [ -e "$f" ] && files="$files
$f"
done
if [ -z "$files" ]; then echo "nothing to back up in $src" >&2; exit 1; fi
# newline-separated list keeps the spaces in "Onion - level_NN.csv" intact
printf '%s\n' "$files" | sed '/^$/d' > ".m0_backup_list"
tar -czf "$out" -T ".m0_backup_list"
rm -f ".m0_backup_list"
if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$out" > "$out.sha256"
else sha256sum "$out" > "$out.sha256"; fi
echo "backup: $out"
echo "checksum: $(cat "$out.sha256")"
echo "next: copy both files off this machine"
