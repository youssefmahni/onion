# Supervisor — controller.py, adaptive policy, lifecycle

`controller.py` is the machine supervisor: it samples the box, computes a thread target, owns the `data_depot funnel` child, and serves the HTTP API + static dashboard assets.

## 1. Sampling (every `--interval` s, default 2)

| Gauge | Source |
|---|---|
| `cores` | `os.cpu_count()` |
| `load1` / `load5` | `/proc/loadavg` |
| `cpu_busy` | delta of `/proc/stat` jiffies |
| `ram_avail_gb` | `/proc/meminfo MemAvailable` |
| `net_rx/tx_mbps` | `/proc/net/dev` (loopback excluded) |
| `link_mbps` | first `e|w|en|wl` iface `/sys/class/net/<n>/speed`; absent → `None` = "no link" (NIC cap only applies when a real link speed is known) |
| `gpu_pct` | `nvidia-smi` → `intel_gpu_top` → Intel i915 sysfs freq ratio → macOS `ioreg -c IOAccelerator` (`Device Utilization %` / `GPU Activity(%)`) |

## 2. Adaptive thread policy (mirrors `depot_core::policy_target`)

```
base = clamp(cores, 2, 16);  t = prev (clamped 1..5000)
load1 > 0.8×cores   → t -= 2   (busy)
load1 < 0.3×cores   → t += 2   (idle; growth capped at base)
RAM  < 1 GB         → t //= 2
link known && rx+tx > 0.7×link → cap 4
gpu_pct > 90        → cap 2
```

- **Hysteresis:** changes apply only after `--hysteresis` s (default 30) — *or immediately when shrinking*.
- **Overrides:** `control/override.txt` caps the target (`min(target, ov)`); `--threads N` pins and disables adaptation; a stale `override.txt` is **deleted at startup**.
- Every change is logged `[policy] threads X -> Y (why)` (last 50 kept in state.json).

The same function is mirrored in: `controller.py`, the dashboard's JS fallback, `tui.py`, and compiled to WASM (`depot_core_wasm.js`, used by the dashboard when present).

## 3. Funnel lifecycle

- `start_funnel()` builds `[build/data_depot, funnel, --limit, --rounds, --concurrency <target>, --model]` + `--root-node <seed>` / `--branch` + `--deep` / `--force`; rotates `funnel_run.log` → `funnel_run.prev.log`.
- Supervision loop:
  - `control/paused` or `control/stopped` (and not already paused) → SIGTERM, 10 s grace, SIGKILL.
  - both flags gone and paused → resume. `control/start.txt` → manual start.
  - **Process exit rc==0 → finished, idle, DO NOT restart** (the keep-alive must not resurrect a clean finish). rc≠0 → 10 s backoff, restart (crash recovery).
- Writes `control/concurrency.txt` every loop (funnel reconciles its pool from it).

## 4. state.json schema

`ts` · `target_threads` · `process {pid, alive, paused}` · `gauges {cores, load1, load5, cpu_busy, ram_avail_gb, net_rx_mbps, net_tx_mbps, link_mbps, gpu_pct}` · `history` (last 120 samples) · `policy` (last 50 events) · `funnel {done, fail, summary, last_line, live_threads, active_layer, node_title, node_path, stage}` (parsed from the log since the last `RUN_START`, 8 MB tail cap) · `opencl` (cached 600 s) · `progress {queued, final, retry, kept, atomic, calls, errors, agents, nodes[]}` · `layers {L0..L9}` · `run {seed, limit, rounds, model}` · `flags {paused, stopped}` · `override`.

Atomic write (`.tmp` + `os.replace`).

## 5. Rebuilds

Every ~60 s (never blocking state updates, at most one in flight): `python3 rebuild_json.py && python3 gen_onion_html.py` with `ONION_ROOT` set — keeps `Onion.json` and `Onion_Review.html` fresh while the funnel grows the tree.

## 6. Security posture (hardened by audit)

- Static serving via an explicit whitelist; everything else 403/404.
- Path normalization check (`fpath.startswith(root)`).
- POST bodies clamped to 4 KB; API JSON only.
- Default bind `127.0.0.1`; no auth on the API — for remote use prefer an SSH tunnel.
- Keys: env-only (never in source), header file `chmod 0600`.
- Policy log is XSS-escaped in all UIs.
