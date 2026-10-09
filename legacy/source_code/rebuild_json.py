#!/usr/bin/env python3
"""Rebuild Onion.json — the single read artifact — from the layer CSVs.

Full 10-level nested tree with per-node metadata:
  level, children count, funnel {kept, atomic, retries, score, ts},
  critique {score, issues, remove, add}

Honours ONION_ROOT (set by the controller); falls back to the script directory.
Writes atomically (tmp + rename) in compact form.
"""
import csv, json, os, time

ROOT = os.environ.get("ONION_ROOT") or os.path.dirname(os.path.abspath(__file__))
FILES = [("level_00", "Onion - level_00.csv"), ("level_01", "Onion - level_01.csv"),
         ("level_02", "Onion - level_02.csv"), ("level_03", "Onion - level_03.csv"),
         ("level_04", "Onion - level_04.csv"), ("level_05", "Onion - level_05.csv"),
         ("level_06", "Onion - level_06.csv"), ("level_07", "Onion - level_07.csv"),
         ("level_08", "Onion - level_08.csv"), ("level_09", "Onion - level_09.csv")]

t0 = time.time()

levels = {}
for lname, fn in FILES:
    path = os.path.join(ROOT, fn)
    if os.path.exists(path):
        with open(path, newline="", encoding="utf-8", errors="replace") as fh:
            rows = list(csv.reader(fh))
    else:
        rows = []
    header, data = (rows[0], rows[1:]) if rows else (["ID", "Title"], [])
    if lname == "level_00" and not data:
        header = ["ID", "Phase"]
    levels[lname] = {"file": fn, "columns": header, "count": len(data),
                     "rows": [dict(zip(header, r)) for r in data]}


def pc(l):
    return levels[l]["columns"][1]


all_nodes = {}          # path -> node dict (every level)


def reg(node, level):
    node["level"] = level
    node["children"] = 0
    all_nodes[node.get("path") or node["id"]] = node
    return node


tree = {}
for r in levels["level_00"]["rows"]:
    tree[r["ID"]] = reg({"id": r["ID"], "path": r["ID"],
                         "name": r.get("Phase", r["ID"]), "artifacts": {}}, 0)
for r in levels["level_01"]["rows"]:
    pid = r[pc("level_01")]; ph = pid.split("-")[0]
    if ph in tree:
        tree[ph]["artifacts"][pid] = reg(
            {"id": r["ID"], "path": pid, "name": r.get("DOC", r.get("Title", pid)), "tasks": {}}, 1)
for r in levels["level_02"]["rows"]:
    pid = r[pc("level_02")]; parts = pid.split("-"); ph, art = parts[0], "-".join(parts[:2])
    if ph in tree and art in tree[ph]["artifacts"]:
        tree[ph]["artifacts"][art]["tasks"][pid] = reg(
            {"id": r["ID"], "path": pid, "name": r[levels["level_02"]["columns"][2]], "subtasks": {}}, 2)
for r in levels["level_03"]["rows"]:
    pid = r[pc("level_03")]; parts = pid.split("-"); ph, task = parts[0], "-".join(parts[:3])
    if ph in tree:
        for art in tree[ph]["artifacts"].values():
            if task in art["tasks"]:
                art["tasks"][task]["subtasks"][pid] = reg(
                    {"id": r["ID"], "path": pid, "name": r["Title"], "details": {}}, 3)
for r in levels["level_04"]["rows"]:
    pid = r[pc("level_04")]; parts = pid.split("-")
    ph, task, st = parts[0], "-".join(parts[:3]), "-".join(parts[:4])
    if ph in tree:
        for art in tree[ph]["artifacts"].values():
            if task in art["tasks"] and st in art["tasks"][task]["subtasks"]:
                art["tasks"][task]["subtasks"][st]["details"][pid] = reg(
                    {"id": r["ID"], "path": pid, "name": r["Title"], "items": {}}, 4)
item_index = {}
td_paths = {r[pc("level_04")]: None for r in levels["level_04"]["rows"]}
for r in levels["level_05"]["rows"]:
    pid = r[pc("level_05")]; parts = pid.split("-")
    ph, task, st, td = parts[0], "-".join(parts[:3]), "-".join(parts[:4]), "-".join(parts[:5])
    parent = pid.rsplit("-", 1)[0]
    node = {"id": r["ID"], "path": pid, "name": r["Title"]}
    if parent in td_paths and ph in tree:
        for art in tree[ph]["artifacts"].values():
            if (task in art["tasks"] and st in art["tasks"][task]["subtasks"]
                    and td in art["tasks"][task]["subtasks"][st]["details"]):
                reg(node, 5)
                art["tasks"][task]["subtasks"][st]["details"][td]["items"][pid] = node
                item_index[pid] = node
    elif parent in item_index:
        reg(node, 5)
        item_index[parent].setdefault("items", {})[pid] = node
        item_index[pid] = node
for lv in ("level_06", "level_07", "level_08", "level_09"):
    lnum = int(lv[-2:])
    for r in levels[lv]["rows"]:
        pid = r[pc(lv)]
        parent = pid.rsplit("-", 1)[0]
        if parent in item_index:
            node = {"id": r["ID"], "path": pid, "name": r["Title"]}
            reg(node, lnum)
            item_index[parent].setdefault("items", {})[pid] = node
            item_index[pid] = node

# children counts
CHILD_KEY = {0: "artifacts", 1: "tasks", 2: "subtasks", 3: "details"}
for n in all_nodes.values():
    n["children"] = len(n.get(CHILD_KEY.get(n["level"], "items"), {}))


def load_tsv(fn):
    path = os.path.join(ROOT, fn)
    if not os.path.exists(path):
        return []
    out = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line:
                out.append(line.split("\t"))
    return out


funnel = {}
for parts in load_tsv("funnel_state.tsv"):
    if len(parts) < 5:
        continue
    try:
        funnel[parts[0]] = {
            "kept": int(parts[1]), "atomic": int(parts[2]), "retries": int(parts[3]),
            "score": float(parts[4]),
            "ts": int(parts[5]) if len(parts) > 5 and parts[5].isdigit() else None}
    except ValueError:
        continue

critique = {}
for parts in load_tsv("funnel_critique.tsv"):
    if len(parts) < 5:
        continue
    try:
        sc = float(parts[1])
    except ValueError:
        sc = None
    critique[parts[0]] = {
        "score": sc, "issues": parts[2],
        "remove": [x for x in parts[3].split(";") if x],
        "add": [x for x in parts[4].split(";") if x]}

for p, m in funnel.items():
    if p in all_nodes:
        all_nodes[p]["funnel"] = m
for p, m in critique.items():
    if p in all_nodes:
        all_nodes[p]["critique"] = m

atomic_paths = set()
for parts in load_tsv("atomic_items.tsv"):
    if parts and parts[0]:
        atomic_paths.add(parts[0])
for p in atomic_paths:
    if p in all_nodes:
        all_nodes[p]["atomic"] = True

by_layer = {f"L{i}": levels[f"level_{i:02d}"]["count"] for i in range(10)}
summary = {
    "total_nodes": sum(by_layer.values()),
    "by_layer": by_layer,
    "funneled": len(funnel),
    "kept_total": sum(m["kept"] for m in funnel.values()),
    "atomic_total": sum(m["atomic"] for m in funnel.values()),
    "atomic_items": len(atomic_paths),
    "source_files": [f for _, f in FILES],
}

out = {
    "name": "OmniCommerce Onion",
    "description": ("Layered work-breakdown: Phase -> Artifact -> Task -> Sub-task -> "
                    "Task Detail -> Item (self-nesting). Each node: id, path, name, "
                    "level, children; funnel/critique metadata where a node was processed."),
    "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "summary": summary,
    "levels": {k: {"file": v["file"], "columns": v["columns"], "count": v["count"]}
               for k, v in levels.items()},
    "tree": tree,
}

dest = os.path.join(ROOT, "Onion.json")
tmp = dest + ".tmp"
with open(tmp, "w", encoding="utf-8") as fh:
    json.dump(out, fh, ensure_ascii=False, separators=(",", ":"))
os.replace(tmp, dest)
print(f"Onion.json OK | nodes {summary['total_nodes']} | funneled {summary['funneled']} | "
      f"{os.path.getsize(dest) / 1e6:.1f} MB | {time.time() - t0:.1f}s")
