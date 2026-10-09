# Operations — deployment, runbook, troubleshooting

## 1. Quick start

```bash
sudo apt update && sudo apt install -y build-essential cmake curl python3 python3-venv
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build                      # 92 tests (84 core + 8 GPU), no network
export OPENROUTER_API_KEY=sk-or-…          # or DEEPSEEK_API_KEY=sk-… ; or .onion_key file
./onion-start P01-IN03-T30                  # start supervisor on a node (10 loops)
./onion-start P01-IN01- --limit 50          # or a branch (trailing dash)
./onion-stop                               # stop (resumes on next start)
./onion-tui                                # terminal dashboard
```

Dashboard: `http://<host>:8080/dashboard.html` (bind is loopback by default).

## 2. Environments

| Env | Details |
|---|---|
| Linux dev box | 10.8.0.67 (VPN), OpenCL via POCL; Intel Iris Xe needs `intel-opencl-icd` (interactive sudo) |
| Mac Pro (primary deployment) | `mac@192.168.1.63` (LAN) / `macpro2` `10.8.0.92` (VPN), macOS 12.7.6, 6C/12T Xeon, 64 GB, headless; two AMD FirePro D500 (OpenCL 1.2, fp64) |
| VPS | only outbound HTTPS to openrouter.ai + GitHub needed (GoogleTest fetched at configure time) |

### macOS quirks (Mac Pro)

- **cmake via pip** (`pip3 install --user cmake`) → prepend `~/Library/Python/3.9/bin` to PATH.
- Scripts must stay **bash 3.2-safe** (no arrays/`mapfile`); `setsid` absent → nohup-only launch.
- `onion_gl` auto-skips (no X11/EGL on macOS).
- **Touch-before-rebuild gotcha:** `rsync -a` preserves source mtimes older than stale `.o` files, so `make` can silently relink old objects. If a source change "doesn't apply", `touch` the sources (or clean-build) before `cmake --build`.

## 3. Funnel run patterns

```bash
./onion-start P06 --limit 100                          # settle 100 loops
./onion-start P06-TK318807 --limit 10000 --rounds 5000 --force   # long expansion run
./onion-start P01-IN01- --threads 500                  # pinned 500 workers (adaptive off)
./onion-start P06 --deep                               # depth-first to L9, settle once
```

- **Resume semantics:** a re-run skips everything in `funnel_state.tsv` — budget is additive across restarts only for unsettled nodes. To re-expand settled nodes use `--force`.
- **`--force` re-runs the whole seeded subtree each round** and filters new candidates against existing children (canonical title dedup) — no duplicates, only genuinely new branches.
- Thread pinning vs override: `--threads N` pins; `/api/threads {"max": N}` only *caps* the adaptive target.
- `--dry` writes nothing (CSVs, TSVs untouched) — use it to preview.

## 4. State & audit files (never delete casually)

- `funnel_state.tsv` — the resume ledger. **Deleting it makes re-runs re-do settled nodes.**
- `funnel_critique.tsv`, `atomic_items.tsv`, `node_llm_report.tsv`, `expand_decisions.csv` — audit + ingest registry.
- `funnel_run.log` / `funnel_run.prev.log` — event stream (controller rotates per run).
- `backups/` — snapshot dirs (gitignored).

**Cleanup of API-failure rows:** failure rows have the signature `kept=0, atomic=0, retries>=1, score=0`. Remove only rows matching it (preferably scoped to a time window) after a systemic outage — never prune successful settles.

## 5. systemd (VPS example)

```ini
[Unit]
Description=Onion supervisor for %i
[Service]
User=onion
WorkingDirectory=/home/onion/PlanningProcess
Environment=OPENROUTER_API_KEY=sk-or-…
ExecStart=/usr/bin/python3 /home/onion/PlanningProcess/controller.py --seed P01-IN01-T30 --limit 50
Restart=on-failure
KillMode=mixed
[Install]
WantedBy=multi-user.target
```

## 6. Ports & exposure

- Default HTTP **8080**, bind **127.0.0.1**; static file server (for Onion.json / review / CSVs) often run as `python3 -m http.server 8090 --bind 0.0.0.0` in the project root.
- The controller API has **no auth** — expose with `--bind 0.0.0.0` only behind ufw / SSH tunnel.

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no API key` | export `OPENROUTER_API_KEY` (or `DEEPSEEK_API_KEY`), or create `.onion_key` |
| Empty model replies everywhere | wrong reasoning knob for the provider (`docs/LLM_AND_PROVIDERS.md §2`); invalid key (401 body); rate limiting — check with a single curl |
| `NODE_FAIL … empty-or-error API body` storm | same as above; check `funnel_run.log` + a raw curl with the exact payload |
| funnel exits rc=0 immediately, 0 calls | everything under the seed is settled — add `--force` (and `--rounds N>1` for budget reuse) |
| funnel exits after 1 node with `--force` | `rounds=1` drained after the first pass — raise `--rounds` |
| Stale build on Mac after rsync | touch sources, rebuild |
| Threads stuck low | stale `control/override.txt` (controller clears at startup) or adaptive policy busy/ram/nic/gpu caps — check `/api/state` policy history |
| `curl: (7) Failed to connect` | no outbound HTTPS (VPS firewall / proxy) |
| HTTP 400 model-not-found | `--model` id not on the endpoint — list via `curl https://openrouter.ai/api/v1/models` |
| Budget overshoot | fixed by strict slot claiming; update the binary |
| pgrep kills unrelated shells | use `onion-stop` (anchored patterns), never raw `pkill -f python` |
