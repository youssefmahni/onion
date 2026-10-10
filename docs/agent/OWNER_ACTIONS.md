# Owner actions (runbooks, copy-paste ready)

Each entry is executable by the owner without asking the agent anything. Status: open | done.

---

### OA-1 · M0 real legacy node · M0 · status: open

Why: The one real DeepSeek node on the corpus copy has never been confirmed to the owner's reviewer. It closes M0 (P1–P7 live proof) and is the precondition for trusting the M0 tag's "verified" annotation.

Prerequisites:
- A machine with the repo (or the legacy binary `legacy/build/data_depot`) and Python 3.
- A DeepSeek API key in `OPENROUTER_API_KEY` (legacy binary reads that variable; the new tree reads `DEEPSEEK_API_KEY`). Do not paste the key anywhere.
- `ORIGINAL_PROJECT`: confirm the folder of the old project. Candidate found by the agent: `~/Desktop/PlanningProcess` (contains `Onion - level_00.csv` … `level_09.csv`). DO NOT run on the live Mac Pro data directory — use a local copy.
- ~1 DeepSeek completion (cost < $0.01).

Steps (copy-paste ready):
```bash
ORIG="$HOME/Desktop/PlanningProcess"        # ← confirm this path first
mkdir -p "$HOME/onion-data"
for f in "$ORIG"/"Onion - level_"*.csv "$ORIG"/funnel_state.tsv "$ORIG"/funnel_critique.tsv \
         "$ORIG"/atomic_items.tsv "$ORIG"/expand_decisions.csv; do
  [ -e "$f" ] && cp "$f" "$HOME/onion-data/"
done
cd "$HOME/Desktop/onion"
cmake -S legacy -B legacy/build && cmake --build legacy/build -j"$(nproc)"
python3 legacy/m0_clean_failure_rows.py "$HOME/onion-data"   # preview only, review before -a
TD=$(python3 - <<'PY'
import csv
with open("$HOME/onion-data/Onion - level_04.csv") as f:
    for row in csv.reader(f):
        if row and row[0].startswith("TD"): print(row[0]); break
PY
)
# choose a TD with no funnel_state.tsv row: grep -c "^$TD" "$HOME/onion-data/funnel_state.tsv" must be 0
OPENROUTER_API_KEY=<the key, inline for this one command only> \
  legacy/build/data_depot funnel --root "$HOME/onion-data" --root-node "$TD" --limit 1
```

Pass criteria: output contains `NODE_DONE` … `FINAL` and `errors=0`. Record the `DUP_PATHS` count and the calls used.

If it fails: collect the full output, the last 50 lines of any funnel log, and the exact command; send them to the agent. Do not delete anything.

Cost / duration: ~1 real call (< $0.01), ~10 minutes.

Note: `m0` already exists on `42ecd8a` ("M0: P1-P7 fixed, one real DeepSeek node verified"). If this run passes, leave the tag as-is and tell the agent the date of the confirmed run; the agent records it. If the owner decides the run supersedes the old one, only the owner moves the tag.

---

### OA-2 · Confirm ORIGINAL_PROJECT path / corpus copy · M0 · status: open

Why: §0 of AUTONOMOUS_GOAL.md left `ORIGINAL_PROJECT` blank, and `~/onion-data` does not exist, so the agent cannot prepare the M0/M4 corpus copy itself.

Steps: reply with the correct path of the old project (e.g. `~/Desktop/PlanningProcess`) or confirm the candidate above. The agent will then create `~/onion-data` and proceed with M0/M4 corpus work.

Cost / duration: none.

---

### OA-3 · Mac Pro self-hosted CI runner · M1 (deferred) · status: open

Why: hosted CI cannot run real fp64 GPU tests nor native macOS 12 tests; the fleet Mac Pros are the intended runner.

Steps: see REBUILD_PLAN.md "Open decisions"; register a runner with `gh` on one Mac Pro, add it to this repo, and tell the agent the runner label.

Cost / duration: ~30 minutes, no money.

---

## Decision records awaiting the owner (Proposed)

- None yet.
