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

### OA-17 · Approve or decline a DuckDB/Parquet export · M8 · status: open

Why: M8 deliberately keeps Parquet export out because DuckDB is a substantial
new dependency. Checkpoint summaries and the JSON dashboard provide the
implemented scale/audit path without it.

Steps: reply with either "keep Parquet out" or "approve DuckDB/Parquet",
including any preferred export directory, retention and schema requirements.

Cost / duration: none to decline; approval needs a follow-up implementation
and dependency review.

---

### OA-8 · Decide review-children storage (decision 0008 §3) · M3 · status: open

Why: The engine stores a NEEDS_REVIEW node's children (with the review) so `review accept` settles without spending more, preserving "spend nothing more on this node". The alternative is re-running the node on accept.

Steps: read docs/decisions/0008-campaign-file-stop-rule-review-children.md §3 and reply with one of: "keep stored children" (record becomes Accepted) or "re-run on accept" (the agent changes the fold for REVIEW/NEEDS_REVIEW).

Cost / duration: none.

---

### OA-9 · M3 real campaign on the corpus copy (optional) · M3 · status: open

Why: The M3 real campaign ran on a synthetic root ("Order Processing") because the corpus copy is pending (OA-2). Running one real TD from the corpus would confirm the gate behaviour on production-shaped nodes.

Prerequisites: OA-2 resolved; `~/onion-data` populated.

Steps: the agent provides a runbook once OA-2 is done; expected cost ≈ $0.02 for 10 nodes at light settings.

Cost / duration: ≤ 150 calls, ≈ $0.02, ~20 minutes.

---

### OA-10 · M4 corpus-copy import and cutover preparation · M4 · status: open

Why: The genesis import was verified on a fixture corpus; the real corpus copy needs OA-2 first. The import itself and the cutover are fully scripted.

Prerequisites: OA-2 resolved (`~/onion-data` populated).

Steps:
1. Run the weekly dry run: `tools/import/weekly_dry_run.sh` (from the repo). Expect `VERIFIED`, `orphans=0`, per-layer counts.
2. Approve the duplicate proposal: copy `~/onion-data/duplicates-proposal.tsv` to `~/onion-data/duplicates-resolution.tsv`, editing the `kept` column where you disagree.
3. The actual cutover follows `docs/CUTOVER.md` §3–§6 when you are ready (no earlier than M6 for the final switch; the dry run is safe now).

Cost / duration: none (offline).

---

### OA-11 · Generate the offline admin key · M4 · status: open

Why: Emergency evictions need the admin key, kept offline. The command exists; only the owner runs it.

Steps:
```bash
data_depot admin keygen --out <path outside any data directory, e.g. a USB drive or password manager export>
```
Store the printed public key with the runbook. Never place the file on a fleet machine.

Cost / duration: none.

---

### OA-12 · M5 Mac Pro profile, service restart and live tuning run · M5 · status: open

Why: Hosted CI cannot run systemd services or touch the Mac Pros; these checks need the real fleet hardware.

Prerequisites: a Mac Pro (Intel, macOS 12) with the repo built; a DeepSeek key for the live run.

Steps:
1. `data_depot node profile --data-dir ~/.local/share/onion` — expect laptop rules v1 (base_weight 0.25, start 4, ceiling 32) unless overridden in node.conf.
2. `node install-service --binary <abs path>/data_depot --data-dir ~/.local/share/onion --base-url https://api.deepseek.com` → copy the plist to ~/Library/LaunchAgents, `launchctl load`, then `kill -9` the node and confirm launchd restarts it (KeepAlive).
3. Live tuning: run a small campaign with real calls; watch the limit stay at the profile start value without 429 storms; report the highest limit reached and the error count.

Cost / duration: ≤ 50 real calls, ~30 minutes.

Pass criteria: the profile shows laptop rules; the node restarts after kill -9; no 429 storm.

If it fails: collect `node profile` output, the plist, and the node log tail.

---

### OA-16 · M7 power pull on a Mac · M7 · status: open

Why: the M7 exit criterion "killing a node mid-run loses no committed work" is proven on the CI scenarios and by the fork-based crash tests, but a REAL power cut on a Mac (pull the cord mid-run) is the owner live check.

Steps:
1. On the Mac Pro: start `data_depot node run` in a fleet, wait for a settle, then pull the power cord.
2. Boot it back, start the node again, and check `node debug-state`: every committed settle is present, the torn tail was cut, and the node re-joins (JOINING → ACTIVE at lag < 100).
3. Record the result here.

Pass criteria: nothing committed is lost; the node is ACTIVE again within one sync round of catching up.

Cost / duration: ~10 minutes.

### OA-15 · M7 Linux machine join · M7 · status: open

Why: the join flow is verified on the CI scenarios and a live two-node loopback smoke, but a REAL machine join (a second physical Linux box joining a running fleet over the LAN) is the owner live check.

Steps:
1. On machine A (running the fleet): `data_depot node invite --pubkey <B's public key> --ttl 24h`.
2. On machine B: `data_depot node join --token @token.json --seed <A's address>`.
3. On B: `data_depot node run --members members.json` and watch it reach `joined: ACTIVE (lag N)`.
4. Record the result here.

Pass criteria: B reaches ACTIVE with lag < 100 and participates in the next campaign.

Cost / duration: ~10 minutes.

### OA-13 · macOS sync 401 diagnosis (multi-process convergence) · M6 (revisited in M7) · status: RESOLVED (agent) — 2026-10-11

Why it was open: on macOS arm64 CI every sync round was rejected with 401 by the signed-request authenticate(), so the multi-process convergence test skipped there.

Resolution (root cause, two layers — full diagnosis in WORKLOG.md):
1. The signed-request scheme had NO per-request nonce: the timestamp was the only varying input. A legitimate transport retry (common on macOS runners) resent byte-identical requests, and the anti-replay cache rejected the retry as a replay → 401. Fixed by adding a fresh random nonce per request attempt to the signed bytes (device ‖ method ‖ path ‖ body-hash ‖ nonce ‖ timestamp) and keying the replay cache on (device, nonce).
2. After that, sync worked but batches failed signature verification on macOS: KeyStore::get read the shared login keychain FIRST. Parallel nodes on one machine and cross-job keychain state on hosted runners made the key material nondeterministic. Fixed by making the 0600 file the source of truth (keychain only as a migration fallback, fail closed on a bad file).

Evidence: hex string-to-sign dumps matched byte-for-byte between client and server on macOS CI; `sync.multi_process_convergence` PASSES on macOS arm64 CI (126 s, run 38167851946, all 8 jobs green); a pinned cross-OS golden signature test (SyncGolden) guards the canonical bytes on every platform.

Removed with the fix: the macOS CI skip in tests/unit/multi_process_test.py and the ONION_MP_ALLOW_MACOS override in .github/workflows/ci.yml.
