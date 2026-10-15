#!/usr/bin/env python3
"""Generate a synthetic corpus database for the dashboard nightly perf job.

Creates <data_dir> with an onion.db holding `--nodes` nodes (default
1,000,000) in a realistic tree: a small root fan-out, then deep branches,
plus settles for a fraction of the leaves. Uses the repository's schema
directly (no dev-import), so the tree endpoints and viewcache see the same
tables the engine writes.

Usage: gen_corpus.py --nodes 1000000 --data-dir <dir>
"""

import argparse
import math
import os
import sqlite3
import sys

SCHEMA = """
CREATE TABLE IF NOT EXISTS nodes (
  id INTEGER PRIMARY KEY, code TEXT NOT NULL, path TEXT NOT NULL UNIQUE,
  parent_id INTEGER, layer INTEGER NOT NULL, title TEXT NOT NULL,
  origin TEXT NOT NULL, hlc INTEGER NOT NULL, tombstone INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS nodes_parent ON nodes(parent_id);
CREATE TABLE IF NOT EXISTS settles (path TEXT PRIMARY KEY, kept INT, atomic INT,
  attempts INT, score TEXT, campaign TEXT, end TEXT, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS attempts (path TEXT, n INT, score TEXT, passed INT,
  tokens_in INT, tokens_cached INT, tokens_out INT, cost TEXT, campaign TEXT,
  hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS critiques (path TEXT, score TEXT, issues TEXT,
  remove TEXT, add_ TEXT, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS reviews (path TEXT PRIMARY KEY, reason TEXT,
  state TEXT, note TEXT, atomic INT NOT NULL DEFAULT 0, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS campaigns (id TEXT PRIMARY KEY, spec TEXT,
  state TEXT, reason TEXT, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS members (device_id TEXT PRIMARY KEY, pubkey BLOB,
  slot INT, state TEXT, incarnation INT, sync_url TEXT, swim_url TEXT,
  profile_version INT, evict_cutoff INT);
CREATE TABLE IF NOT EXISTS profiles (device_id TEXT, version INT, prev TEXT,
  reason TEXT, changed TEXT, spec TEXT, rules_version INT, schema INT, hlc INT,
  PRIMARY KEY (device_id, version));
CREATE TABLE IF NOT EXISTS applied (origin TEXT PRIMARY KEY, seq INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS batches (origin TEXT NOT NULL, number INTEGER NOT NULL,
  hash TEXT NOT NULL, PRIMARY KEY (origin, number));
CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS stats_levels(layer INTEGER PRIMARY KEY,
  nodes INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS stats_status(status TEXT PRIMARY KEY, n INTEGER NOT NULL);
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nodes", type=int, default=1_000_000)
    parser.add_argument("--data-dir", required=True)
    args = parser.parse_args()

    target = args.nodes
    data_dir = args.data_dir
    os.makedirs(data_dir, exist_ok=True)
    db_path = os.path.join(data_dir, "onion.db")
    if os.path.exists(db_path):
        os.remove(db_path)

    connection = sqlite3.connect(db_path)
    connection.executescript(SCHEMA)
    connection.execute("PRAGMA journal_mode=WAL;")
    connection.execute("PRAGMA synchronous=NORMAL;")

    # Shape: ~200 roots, each with a fan-out of ~8 per level, depth ~6 —
    # that fills 200 * (8^6) leaves quickly; cap at `target`.
    roots = 200
    fanout = 8
    max_layer = 9
    rows = []
    next_id = 1

    def add(path, code, parent_id, layer, title):
        nonlocal next_id
        rows.append((next_id, code, path, parent_id, layer, title,
                     "dev0000000000000001", layer + 1, 0))
        next_id += 1

    layer_paths = [[]]
    for r in range(roots):
        path = "P%02d" % r
        add(path, path, None, 0, "Root %02d" % r)
        layer_paths[0].append(path)

    for layer in range(1, max_layer + 1):
        layer_paths.append([])
        for parent in layer_paths[layer - 1]:
            for f in range(fanout):
                if next_id - 1 >= target:
                    break
                code = "%s%03d" % (parent, f)
                add(code, code, None, layer, "Node %s" % code)
                layer_paths[layer].append(code)
            if next_id - 1 >= target:
                break
        if next_id - 1 >= target:
            break

    # parent_id resolution in one pass
    path_to_id = {}
    for row in rows:
        path_to_id[row[2]] = row[0]
    for index, row in enumerate(rows):
        parent_path = "-".join(row[2].split("-")[:-1]) if "-" in row[2] else ""
        if parent_path in path_to_id:
            rows[index] = row[:3] + (path_to_id[parent_path],) + row[4:]

    connection.executemany(
        "INSERT INTO nodes(id, code, path, parent_id, layer, title, origin, "
        "hlc, tombstone) VALUES (?,?,?,?,?,?,?,?,?);",
        rows,
    )
    total = len(rows)

    # Settle a fraction of the deepest leaves.
    settled = 0
    deepest = layer_paths[-1] if layer_paths else []
    for i, path in enumerate(deepest):
        if i % 3 != 0:
            continue
        connection.execute(
            "INSERT INTO settles(path, kept, atomic, attempts, score, "
            "campaign, end, hlc, origin) VALUES (?,?,?,?,?,?,?,?,?)",
            (path, 1, 0, 3, "0.6", "bench", "children", 1,
             "dev0000000000000001"),
        )
        settled += 1

    # The derived counters (§6.1) exactly as the engine recomputes them.
    connection.execute(
        "INSERT INTO stats_levels(layer, nodes) SELECT layer, COUNT(*) FROM "
        "nodes WHERE tombstone = 0 GROUP BY layer;"
    )
    connection.execute(
        "INSERT INTO stats_status(status, n) SELECT end, COUNT(*) FROM "
        "settles WHERE end IS NOT NULL AND end != '' GROUP BY end;"
    )
    connection.execute("INSERT INTO schema_version(version) VALUES (5);")
    connection.commit()
    connection.close()
    print("generated %d nodes (%d settled) in %s" % (total, settled, db_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
