#!/usr/bin/env python3
"""W5 variant driver: run one campaign on the real API to completion.

Usage:
    run_variant.py --data-dir DIR --campaign-json FILE [--settle-count N]
                   [--api-port P] [--sync-port P] [--accept-reviews]

Spawns tools/llm_proxy.py (recording cassette.jsonl into the data dir) and
`data_depot node`, polls the settles table, and — when the campaign stalls
with every remaining path under an open review — auto-accepts the reviews
(mirroring the human operator step of the W5 baseline, W5_BASELINE.md).
Stops the campaign when N settles are reached and exits non-zero on error.
The campaign id is read from the campaign JSON; stop is final (0010), so a
fresh id is needed for every run.

Note: the proxy forwards to the real API with DEEPSEEK_API_KEY from its
own environment; when launched under `systemd-run --user` the environment
is NOT inherited from the caller's shell, so pass
`--setenv=DEEPSEEK_API_KEY=...` explicitly (observed on this machine).
"""

import argparse
import json
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def count_settles(db_path):
    db = sqlite3.connect(db_path)
    try:
        return db.execute("SELECT COUNT(*) FROM settles").fetchone()[0]
    finally:
        db.close()


def open_review_paths(db_path):
    db = sqlite3.connect(db_path)
    try:
        return [r[0] for r in db.execute(
            "SELECT path FROM reviews WHERE state = 'open' OR state = ''")]
    finally:
        db.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--campaign-json", required=True)
    ap.add_argument("--settle-count", type=int, default=12)
    ap.add_argument("--api-port", type=int, default=18113)
    ap.add_argument("--sync-port", type=int, default=18213)
    ap.add_argument("--accept-reviews", action="store_true")
    args = ap.parse_args()

    data = Path(args.data_dir).resolve()
    binary = ROOT / "build" / "data_depot"
    proxy = ROOT / "tools" / "llm_proxy.py"
    campaign = json.loads(Path(args.campaign_json).read_text())
    campaign_id = campaign["id"]
    db_path = data / "onion.db"

    proxy_port = free_port()
    proxy_proc = subprocess.Popen(
        [sys.executable, str(proxy), "--port", str(proxy_port),
         "--record", str(data / "cassette.jsonl")],
        stdout=open(data / "proxy.log", "w"), stderr=subprocess.STDOUT)
    time.sleep(1.5)
    if proxy_proc.poll() is not None:
        print("proxy failed to start", file=sys.stderr)
        return 1

    node = subprocess.Popen(
        [str(binary), "node", "run", "--data-dir", str(data),
         "--api-port", str(args.api_port), "--sync-port", str(args.sync_port),
         "--base-url", f"http://127.0.0.1:{proxy_port}",
         "--model", "deepseek-flash"],
        stdout=open(data / "node.log", "w"), stderr=subprocess.STDOUT)

    deadline = time.time() + 6 * 3600
    last_settles = count_settles(db_path)
    last_progress = time.time()
    try:
        while True:
            time.sleep(10)
            settles = count_settles(db_path)
            if settles != last_settles:
                last_settles = settles
                last_progress = time.time()
            print(f"settled {settles}/{args.settle_count}", flush=True)
            if settles >= args.settle_count:
                break
            if args.accept_reviews and time.time() - last_progress > 120:
                reviews = open_review_paths(db_path)
                # The engine is idle (no new settles for 2 minutes) and
                # paths are under open review: act as the human operator
                # (W5_BASELINE.md) and accept the stored children.
                for path in reviews:
                    subprocess.run(
                        [str(binary), "review", "accept", path,
                         "--note", "W5 variant: below floor, accepting "
                                   "stored children",
                         "--data-dir", str(data)],
                        timeout=60)
                last_progress = time.time()
            if time.time() > deadline:
                print("deadline", file=sys.stderr)
                return 1
        subprocess.run([str(binary), "campaign", "stop", campaign_id,
                        "--data-dir", str(data)], timeout=60)
        print("done", flush=True)
        return 0
    finally:
        for proc in (node, proxy_proc):
            try:
                proc.terminate()
                proc.wait(timeout=10)
            except Exception:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
