#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate funnel_results.html from a funnel test run's artifacts."""
import csv, html
from collections import defaultdict

def esc(s):
    return html.escape(s or "")

# read critique rows
crit = []
for line in open("funnel_critique.tsv"):
    parts = line.rstrip("\n").split("\t")
    if len(parts) >= 3:
        crit.append({"path": parts[0], "score": parts[1], "issues": parts[2],
                     "remove": parts[3] if len(parts) > 3 else "",
                     "add": parts[4] if len(parts) > 4 else ""})

# generated subtree rows (test artifacts under the root node)
levels = {}
for lv in range(2, 10):
    fn = f"Onion - level_{lv:02d}.csv"
    try:
        rows = list(csv.reader(open(fn, newline="")))[1:]
    except FileNotFoundError:
        rows = []
    for r in rows:
        if r[1].startswith("P01-IN01-TK10"):
            levels.setdefault(lv, []).append(r)

state_rows = 0
try:
    state_rows = sum(1 for _ in open("funnel_state.tsv"))
except FileNotFoundError:
    pass

summary_line = "?"
try:
    for line in open("funnel_run.log"):
        if line.startswith("funneled="):
            summary_line = line.strip()
except FileNotFoundError:
    pass

score_col = {"low": "#f43f5e", "mid": "#f59e0b", "high": "#22c55e"}
def scol(v):
    if v < 0.4: return score_col["low"]
    if v < 0.7: return score_col["mid"]
    return score_col["high"]

def gauge(score, label):
    try:
        v = float(score)
    except ValueError:
        v = 0.0
    pct = int(v * 100)
    col = scol(v)
    return f'''<div class="gauge"><div class="bar"><div class="fill" style="width:{pct}%;background:{col}"></div></div>
    <span class="gv" style="color:{col}">{label}</span></div>'''

# group critique rows by path
by_path = defaultdict(list)
for c in crit:
    by_path[c["path"]].append(c)

cards = []
for path, entries in sorted(by_path.items()):
    n = len(entries)
    last = entries[-1]
    retried = n > 1
    cards.append(f'''
    <div class="node {'retried' if retried else ''}">
      <div class="node-head"><span class="path">{esc(path)}</span>
        <span class="tag">{n} critique round{'' if n==1 else 's'}{' · RETRIED' if retried else ''}</span></div>
      {gauge(last["score"], f'{last["score"]}')}
      <div class="issues">{esc(last["issues"]) or '<span class="none">no issues reported</span>'}</div>
    </div>''')

level_blocks = ""
for lv in sorted(levels):
    rows = levels[lv]
    items = "".join(f'<li><span class="tid">TK</span>{esc(r[2])}<span class="pathp">{esc(r[1])}</span></li>'
                    for r in rows)
    level_blocks += f'''<div class="lvl"><h3>level_{lv:02d} — generated children ({len(rows)})</h3><ul>{items}</ul></div>'''

html_doc = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Funnel Test Results — Identify Problem or Opportunity (root)</title>
<style>
:root{{--bg:#070b14;--panel:#101a2e;--panel2:#15213a;--line:#24344f;--ink:#e8eef9;--muted:#8fa0bf;--accent:#5b8def}}
*{{box-sizing:border-box;margin:0;padding:0}}
body{{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--ink);padding:40px 22px}}
.wrap{{max-width:1000px;margin:0 auto}}
h1{{font-size:24px}} h2{{font-size:18px;margin:26px 0 12px}} .sub{{color:var(--muted);font-size:13.5px}}
.summary{{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}}
.s{{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:12px 16px}}
.s b{{font-size:20px;color:var(--accent);display:block}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(290px,1fr));gap:12px;margin-top:14px}}
.node{{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px}}
.node.retried{{border-color:#f59e0b}}
.node-head{{display:flex;justify-content:space-between;gap:8px;align-items:center;margin-bottom:10px}}
.path{{font-family:ui-monospace,monospace;font-size:12px;color:#9fb2d8;word-break:break-all}}
.tag{{font-size:10.5px;font-weight:700;color:#fbbf24;white-space:nowrap}}
.gauge .bar{{height:8px;background:var(--panel2);border-radius:6px;overflow:hidden}}
.gauge .fill{{height:100%;border-radius:6px}}
.gauge .gv{{display:block;text-align:right;font-size:13px;font-weight:700;margin-top:4px}}
.issues{{color:var(--muted);font-size:13px;margin-top:10px;line-height:1.55}}
.none{{opacity:.5;font-style:italic}}
.lvl{{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px;margin-top:12px}}
.lvl ul{{list-style:none;margin-top:8px}}
.lvl li{{padding:5px 0;border-bottom:1px dashed var(--line);font-size:13.5px;display:flex;gap:10px;align-items:center}}
.lvl li:last-child{{border-bottom:none}}
.tid{{color:var(--accent);font-family:ui-monospace,monospace;font-size:10px;border:1px solid var(--line);padding:1px 6px;border-radius:6px}}
.pathp{{margin-left:auto;color:var(--muted);font-family:ui-monospace,monospace;font-size:10px;opacity:.7}}
.note{{background:var(--panel);border:1px solid #7c3aed;border-radius:12px;padding:14px;margin-top:18px;color:#c4b5fd;font-size:13.5px;line-height:1.6}}
.legend{{display:flex;gap:14px;margin-top:8px;color:var(--muted);font-size:12px}}
.dot{{width:9px;height:9px;border-radius:3px;display:inline-block;margin-right:5px}}
</style></head><body><div class="wrap">
<h1>Funnel Test Results</h1>
<p class="sub">Seed: root node <b>P01-IN01 — Identify Problem or Opportunity</b> · limit 12 loops · concurrency 8 · google/gemini-2.5-flash-lite via OpenRouter (stateless, low reasoning)</p>
<div class="summary">
  {''.join(f'<div class="s"><b>{esc(v)}</b>{esc(k)}</div>' for k, v in [('funneled', summary_line.split("funneled=")[1].split(" ")[0] if "funneled=" in summary_line else "—"),
   ('kept', summary_line.split("kept=")[1].split(" ")[0] if "kept=" in summary_line else "—"),
   ('atomic', summary_line.split("atomic=")[1].split(" ")[0] if "atomic=" in summary_line else "—"),
   ('LLM calls', summary_line.split("calls=")[1].split(" ")[0] if "calls=" in summary_line else "—"),
   ('self-corrections (retries)', summary_line.split("retried=")[1].split(" ")[0] if "retried=" in summary_line else "—"),
   ('errors', summary_line.split("errors=")[1].strip() if "errors=" in summary_line else "—")])}
</div>
<div class="legend">
  <span><span class="dot" style="background:#f43f5e"></span>score &lt; 0.4 — weak</span>
  <span><span class="dot" style="background:#f59e0b"></span>0.4–0.7 — retry</span>
  <span><span class="dot" style="background:#22c55e"></span>≥ 0.7 — accepted</span>
</div>
<h2>Critique Agent verdicts per node ({len(crit)} rounds, {len(by_path)} nodes)</h2>
<div class="grid">{''.join(cards)}</div>
<h2>Generated knowledge rows ({sum(len(v) for v in levels.values())})</h2>
{level_blocks}
<div class="note">The self-copy defect seen earlier ("Identify Problem or Opportunity" generated as its own child) is
already fixed by a guard (a node can never be its own child) — this run was executed after the fix, so none of the
generated rows should repeat a parent title.</div>
</div></body></html>"""

open("funnel_results.html", "w", encoding="utf-8").write(html_doc)
print("wrote funnel_results.html |", summary_line, "| state rows:", state_rows)
