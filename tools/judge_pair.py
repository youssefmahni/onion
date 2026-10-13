#!/usr/bin/env python3
"""Blind pairwise judge for the W5 variants (one API call per pair).

Compares the baseline children of a path with a variant's children for the
same path; the side order is randomized per pair with a fixed seed, the
prompt is tools/judge_prompt.txt, and every result is appended to a JSONL.

Usage:
    judge_pair.py --baseline-dir DIR --variant-dir DIR --path PATH
                  --out FILE [--seed N] [--base-url URL] [--model M]
                  [--api-key-env NAME]
"""

import argparse
import json
import os
import random
import sqlite3
import sys
import urllib.request


def children_for(db_path, path):
    db = sqlite3.connect(db_path)
    try:
        return [r[0] for r in db.execute(
            "SELECT title FROM nodes WHERE path LIKE ? ORDER BY path",
            (path + "-%",))]
    finally:
        db.close()


def call_api(base_url, model, key, prompt, node_title, titles_a, titles_b):
    body = {
        "model": model,
        "temperature": 0.0,
        "effort": "low",
        "max_tokens": 512,
        "messages": [
            {"role": "system", "content": prompt},
            {"role": "user",
             "content": node_title + "\n\nSet A:\n" +
                        "\n".join(f"- {t}" for t in titles_a) +
                        f"\n\nSet B:\n" +
                        "\n".join(f"- {t}" for t in titles_b)},
        ],
    }
    request = urllib.request.Request(
        f"{base_url}/chat/completions", data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.loads(response.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline-dir", required=True)
    ap.add_argument("--variant-dir", required=True)
    ap.add_argument("--path", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=20261011)
    ap.add_argument("--base-url", default="http://127.0.0.1:41587/v1")
    ap.add_argument("--model", default="deepseek-flash")
    ap.add_argument("--api-key-env", default="DEEPSEEK_API_KEY")
    args = ap.parse_args()

    key = os.environ.get(args.api_key_env)
    if not key:
        print(f"{args.api_key_env} is not set", file=sys.stderr)
        return 1
    prompt = open(Path := __import__("pathlib").Path(
        __file__).parent / "judge_prompt.txt").read()

    baseline = children_for(Path := __import__("pathlib").Path(
        args.baseline_dir) / "onion.db", args.path)
    variant = children_for(__import__("pathlib").Path(
        args.variant_dir) / "onion.db", args.path)
    db = sqlite3.connect(__import__("pathlib").Path(
        args.baseline_dir) / "onion.db")
    try:
        node_title = db.execute("SELECT title FROM nodes WHERE path = ?",
                                (args.path,)).fetchone()
        node_title = node_title[0] if node_title else args.path
    finally:
        db.close()
    node_title = "Node: " + node_title
    if not baseline or not variant:
        print(f"empty set for {args.path} (baseline={len(baseline)} "
              f"variant={len(variant)})", file=sys.stderr)
        return 1

    rng = random.Random(args.seed)
    flipped = rng.random() < 0.5
    first, second = (variant, baseline) if flipped else (baseline, variant)
    reply = call_api(args.base_url, args.model, key, prompt, node_title,
                     first, second)
    message = reply["choices"][0]["message"]
    # DeepSeek flash streams most of its output into reasoning_content;
    # use it when content is empty.
    winner_raw = message.get("content") or message.get("reasoning_content", "")
    import re
    m = re.search(r'\{\s*"winner"\s*:\s*"(A|B|tie)"', winner_raw)
    if m:
        winner_raw = m.group(0).replace('"winner"', '"winner"')
        # normalise to {"winner": X}
        winner_raw = '{"winner": "' + m.group(1) + '"}'
    record = {
        "path": args.path,
        "model": args.model,
        "seed": args.seed,
        "baseline_first": not flipped,
        "baseline_count": len(baseline),
        "variant_count": len(variant),
        "judge_raw": winner_raw,
        "usage": reply.get("usage", {}),
    }
    with open(args.out, "a") as out:
        out.write(json.dumps(record) + "\n")
    print(json.dumps({"path": args.path, "judge": winner_raw}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
