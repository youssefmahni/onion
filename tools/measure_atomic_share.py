#!/usr/bin/env python3
"""DR 0021 measurement: verified atomic share on the W5 sample.

For each settled path with children, the kept titles come from the
store and the atomic labels from the recorded synthesizer reply; ONE
batched verification call per path (through the budget-enforcing
proxy) returns which of the labelled children admit no decomposition.
"""

import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

PROXY = Path(__file__).resolve().parent / "llm_proxy.py"

RUNS = [
    ("w54-base4", "/home/youssef/w54/base4",
     ["P01-OUT01-T12-ST31-TD1053-TK125569-TK144983"]),
    ("w54-v3", "/home/youssef/w54/v3",
     ["P01-OUT01-T12-ST31-TD1053-TK125569-TK144983"]),
    ("w54-v1", "/home/youssef/w54/v1",
     ["P01-OUT01-T12-ST31-TD1053-TK125569-TK144983"]),
    ("w54-base", "/home/youssef/w54/base",
     ["P01-IN02-T18-TK225651-TK226195"]),
    ("w5-base", "/home/youssef/w5-base/data",
     ["P01-IN03-T30-TK232632-TK245527",
      "P01-IN01-T01-ST09-TD1356-TK70717",
      "P01-OUT01-T15-ST39-TD1093-TK137410-TK186242",
      "P01-IN01-T01-ST01-TD1320-TK68358-TK83748"]),
]


def prompt(node_title, labelled):
    lines = ["The node \"" + node_title +
             "\" was expanded and these children were labelled atomic "
             "(cannot be decomposed further):"]
    for child in labelled:
        lines.append("- " + child)
    lines.append("Verify each against the node: which of these admit NO "
                 "further decomposition into simpler sub-items for an "
                 "AI-agent knowledge base? Answer ONLY with JSON "
                 "{\"admits_none\": [ their titles VERBATIM ]}.")
    return "\n".join(lines)


def main():
    import sqlite3
    key = os.environ.get("DEEPSEEK_API_KEY", "")
    if not key:
        print("no key", file=sys.stderr)
        return 1
    record = "/home/youssef/w5-verify.jsonl"
    Path(record).unlink(missing_ok=True)
    proxy = subprocess.Popen(
        [sys.executable, str(PROXY), "--port", "0", "--record", record],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        env=dict(os.environ, DEEPSEEK_API_KEY=key))
    port = int(proxy.stdout.readline().strip())
    time.sleep(0.5)

    rows = []
    try:
        for tag, directory, paths in RUNS:
            db = sqlite3.connect(Path(directory) / "onion.db")
            cassette = Path(directory).parent / tag / "cassette.jsonl"
            if not cassette.exists():
                cassette = Path(directory) / "cassette.jsonl"
            for path in paths:
                kept = [r[0] for r in db.execute(
                    "SELECT title FROM nodes WHERE path LIKE ?", (path + "-%",))]
                title_row = db.execute(
                    "SELECT title FROM nodes WHERE path = ?", (path,)).fetchone()
                if not kept or not title_row:
                    continue
                node_title = title_row[0]
                # labels from the recorded synthesizer replies for this path
                labelled = []
                if cassette.exists():
                    for line in cassette.read_text(encoding="utf-8").splitlines():
                        try:
                            entry = json.loads(line)
                        except Exception:
                            continue
                        body = entry.get("body") or ""
                        if "select the final children set" not in body:
                            continue
                        resp = json.loads(entry.get("response") or "{}")
                        m = (resp.get("choices") or [{}])[0].get("message", {})
                        content = m.get("content") or m.get(
                            "reasoning_content") or ""
                        items = json.loads(content).get("items") or json.loads(
                            content).get("children") or []
                        titles = [i["title"] for i in items if isinstance(i, dict)]
                        kept_set = set(kept)
                        if not kept_set.issubset(set(titles)):
                            continue
                        labelled = [i["title"] for i in items
                                    if isinstance(i, dict)
                                    and i.get("atomic") is True
                                    and i["title"] in kept_set]
                        break
                if not labelled:
                    rows.append((tag, path, len(kept), 0, 0.0))
                    continue
                body = json.dumps({
                    "model": "deepseek-flash", "temperature": 0.0,
                    "reasoning_effort": "none",
                    "max_tokens": max(128, 10 * len(labelled)),
                    "messages": [{"role": "system", "content": "x"},
                                 {"role": "user",
                                  "content": prompt(node_title, labelled)}]})
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/chat/completions",
                    data=body.encode(),
                    headers={"Content-Type": "application/json",
                             "Authorization": "Bearer " + key})
                for attempt in range(3):
                    try:
                        with urllib.request.urlopen(request, timeout=180) as r:
                            payload = r.read().decode("utf-8", "replace")
                        break
                    except urllib.error.HTTPError as e:
                        if e.code in (429, 500, 502, 503) and attempt < 2:
                            time.sleep(2)
                            continue
                        raise
                message = json.loads(payload)["choices"][0]["message"]
                content = message.get("content") or ""
                match = re.search(r'"admits_none"\s*:\s*(\[[^\]]*\])', content)
                admits = set()
                if match:
                    try:
                        admits = set(json.loads(match.group(1)))
                    except Exception:
                        pass
                verified = sum(1 for title in labelled
                               if title.strip() in admits)
                rows.append((tag, path, len(kept), verified,
                             verified / len(kept)))
                print(f"{tag} {path[-20:]} kept {len(kept)} labelled "
                      f"{len(labelled)} verified {verified} "
                      f"share {verified/len(kept):.2f}", flush=True)
    finally:
        proxy.terminate()

    total_kept = sum(r[2] for r in rows)
    total_verified = sum(r[3] for r in rows)
    print(f"\nTOTAL kept {total_kept} verified {total_verified} "
          f"share {total_verified/total_kept:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
