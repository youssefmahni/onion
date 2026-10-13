#!/usr/bin/env python3
"""DR 0020 validation: re-score the recorded W5 critique calls with the
rubric prompt (no new candidate generation).

For every recorded critique call (identified by the request text), the
candidates and the node context are extracted from the recorded prompt,
the NEW rubric prompt is built, and the real API re-scores it at
temperature 0. Results are appended to a JSONL and also printed as the
validation table (AUC vs the blind judge, tied-score rate, first-attempt
pass rate at 0.7).

All real calls go through tools/llm_proxy.py, which enforces the
project budget caps itself.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROXY = ROOT / "tools" / "llm_proxy.py"

RUBRIC = (
    "Critique this candidate set for the node. This is a RAW CANDIDATE "
    "POOL meant to be filtered: score it on how many candidates are "
    "usable direct children of \"{title}\" (precision), not on whether "
    "every candidate is perfect. Check: (1) hallucinations or off-topic "
    "items, (2) duplicates, (3) important missing categories, (4) weak or "
    "vague items, (5) compliance (Title Case, no questions, no emoji).\n"
    "Calibrate against these anchors for the node \"Order Capture\":\n"
    "- Score 0.8 example: [Order Entry Form, Payment Authorization, "
    "Shipping Method Selection, Order Confirmation, Order Status "
    "Tracking] — every item is a direct part, none weak.\n"
    "- Score 0.5 example: [Order Entry Form, Payment Authorization, "
    "Discount Codes, Customer Loyalty Program, Refund Policy] — the "
    "first two are direct parts, the rest belong to sibling nodes.\n"
    "- Score 0.2 example: [Selling Strategies, Customer Service, Best "
    "Practices, E-commerce Trends, Tips And Tricks] — mostly off-topic.\n"
    "Apply the same standard to this node. Candidates:\n{candidates}\n"
    "Return JSON {{\"score\": 0.0-1.0 usable-candidate ratio, \"remove\": "
    "[ items to drop ], \"add\": [ missing items to include ], "
    "\"issues\": \"short list of problems\"}}.\n{node_line}"
)

CASSETTES = sorted(Path(p) for p in [
    "/home/youssef/w5-base/data/cassette.jsonl",
    "/home/youssef/w5-variants/v1/cassette.jsonl",
    "/home/youssef/w5-variants/v2/cassette.jsonl",
    "/home/youssef/w5-variants/v3/cassette.jsonl",
    "/home/youssef/w5-variants/v4/cassette.jsonl",
    "/home/youssef/w5-variants/v5/cassette.jsonl",
    "/home/youssef/w5-variants/v6/cassette.jsonl",
    "/home/youssef/w5-variants/v1k/cassette.jsonl",
    "/home/youssef/w5-variants/v2k/cassette.jsonl",
    "/home/youssef/w5-variants/v3k/cassette.jsonl",
    "/home/youssef/w5-variants/v4k/cassette.jsonl",
    "/home/youssef/w5-variants/v5k/cassette.jsonl",
    "/home/youssef/w5-variants/v6k/cassette.jsonl",
    "/home/youssef/w5-variants/v7-full/cassette.jsonl",
    "/home/youssef/w5-variants/v7-light/cassette.jsonl",
    "/home/youssef/w54/base/cassette.jsonl",
    "/home/youssef/w54/v3/cassette.jsonl",
    "/home/youssef/w54/v1/cassette.jsonl",
    "/home/youssef/w54/base4/cassette.jsonl",
] if Path(p).exists())


def extract(user):
    """Return (title, candidates, node_line) from a recorded critique
    prompt, or None when the shape is unrecognized."""
    title = re.search(r'direct children of "(.*?)"', user)
    node_line = re.search(r"(Node: .*Title: .*)$", user, re.S)
    match = re.search(r"Candidates:\n(.*)\nReturn JSON", user, re.S)
    if not title or not match or not node_line:
        return None
    candidates = [line.strip() for line in match.group(1).splitlines()
                  if line.strip()]
    if not candidates:
        return None
    return title.group(1), candidates, node_line.group(1)


def call_api(base_url, model, key, prompt):
    body = json.dumps({
        "model": model, "temperature": 0.0,
        "reasoning_effort": "none", "max_tokens": 6000,
        "messages": [{"role": "system",
                      "content": "You are a strict knowledge-curation "
                                 "assistant for a hierarchical AI-agent "
                                 "knowledge base."},
                     {"role": "user", "content": prompt}],
    })
    request = urllib.request.Request(
        f"{base_url}/chat/completions", data=body.encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {key}"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                payload = response.read().decode("utf-8", "replace")
            break
        except urllib.error.HTTPError as error:
            if error.code in (429, 500, 502, 503) and attempt < 2:
                time.sleep(2 * (attempt + 1))
                continue
            raise
    message = json.loads(payload)["choices"][0]["message"]
    content = (message.get("content") or "") + "\n" + \
        (message.get("reasoning_content") or "")
    return content, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default="deepseek-flash")
    args = ap.parse_args()

    key = os.environ.get("DEEPSEEK_API_KEY", "")
    if not key:
        print("DEEPSEEK_API_KEY is not set", file=sys.stderr)
        return 1

    proxy = subprocess.Popen(
        [sys.executable, str(PROXY), "--port", "0", "--record", args.out],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        env=dict(os.environ, DEEPSEEK_API_KEY=key))
    port = int(proxy.stdout.readline().strip())
    base_url = f"http://127.0.0.1:{port}/v1"
    time.sleep(0.5)

    out_path = Path(args.out)
    seen = set()
    rows = []  # (tag, path, attempt_index, score)
    try:
        for cassette in CASSETTES:
            # The run directory name (baseline's cassette lives in
            # <dir>/data/, the rest directly in <dir>).
            tag = cassette.parent.name
            if tag == "data":
                tag = cassette.parent.parent.name
            path_order = {}
            for line in cassette.read_text(encoding="utf-8").splitlines():
                try:
                    entry = json.loads(line)
                except Exception:
                    continue
                if "/chat/completions" not in entry.get("path", ""):
                    continue
                body = entry.get("body") or ""
                if "Critique this candidate set" not in body:
                    continue
                request = json.loads(body)
                user = request["messages"][-1]["content"]
                parts = extract(user)
                if parts is None:
                    continue
                title, candidates, node_line = parts
                prompt = RUBRIC.format(title=title,
                                       candidates="\n".join(candidates),
                                       node_line=node_line)
                key_hash = (tag, title, tuple(candidates))
                if key_hash in seen:
                    continue
                seen.add(key_hash)
                content, payload = call_api(base_url, args.model, key, prompt)
                score = re.search(
                    r'"score"\s*:\s*(-?\d+(?:\.\d+)?)\s*[,}]', content)
                score = float(score.group(1)) if score else None
                # attempt index: order of appearance per (tag, title)
                idx = path_order.get((tag, title), 0) + 1
                path_order[(tag, title)] = idx
                rows.append((tag, title, idx, score))
                print(f"{tag} {title[:40]} attempt {idx} -> {score}",
                      flush=True)
    finally:
        proxy.terminate()

    with out_path.open("a", encoding="utf-8") as out:
        for tag, title, idx, score in rows:
            out.write(json.dumps({"tag": tag, "title": title,
                                  "attempt": idx, "score": score}) + "\n")

    # First-attempt pass rate at 0.7 (non-leaf nodes only = any scored).
    firsts = [r for r in rows if r[2] == 1]
    passed = [r for r in firsts if r[3] is not None and r[3] >= 0.7]
    print(f"\nfirst-attempt critiques: {len(firsts)} "
          f"at >=0.7: {len(passed)} ({len(passed)/max(1,len(firsts)):.0%})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
