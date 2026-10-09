#!/usr/bin/env python3
"""Generate Onion_Review.html — a D3 collapsible tidy tree over Onion.json.

Style: https://observablehq.com/@d3/tree/2 (d3-hierarchy tidy tree, click to
expand/collapse). Adapted for a 300k+ node depot:
  - nodes are materialized lazily on expand (nothing is laid out until visible)
  - fan-out is capped at CHUNK children per node; a "… N more" node loads the
    next chunk on click
  - labels are drawn only when the visible node count is small; hover shows a
    tooltip with path, level, children, funnel and critique metadata
  - d3 is inlined from third_party/d3.v7.min.js (single-file, works from file://)
"""
import json, os

ROOT = os.environ.get("ONION_ROOT") or os.path.dirname(os.path.abspath(__file__))
data = json.load(open(os.path.join(ROOT, "Onion.json"), encoding="utf-8"))
tree = data["tree"]
levels = data["levels"]


def human_size(n):
    u = "B"
    for nxt in ("KB", "MB", "GB"):
        if n < 1024:
            break
        n /= 1024
        u = nxt
    return f"{n:.0f} {u}" if u != "B" else f"{n} B"


json_size = human_size(os.path.getsize(os.path.join(ROOT, "Onion.json")))
payload = json.dumps({"tree": tree, "levels": levels,
                      "summary": data.get("summary", {}),
                      "json_size": json_size}, ensure_ascii=False)
# Titles are LLM-generated and untrusted: never let "</script>" (or U+2028/29
# line separators) break out of the inline script block.
payload = payload.replace("</", "<\\/").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")

d3_path = os.path.join(ROOT, "third_party", "d3.v7.min.js")
if os.path.exists(d3_path):
    d3_js = open(d3_path, encoding="utf-8").read()
else:  # fallback: CDN (needs network)
    d3_js = ""
    print("warning: third_party/d3.v7.min.js missing - falling back to CDN")

HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OmniCommerce Onion — Visual Review (D3 tree)</title>
<style>
:root{
  --bg:#0f1523; --panel:#182136; --panel2:#1f2b47; --ink:#e8edf7; --muted:#9aa8c7;
  --accent:#5b8def; --line:#2b3a5e;
}
*{box-sizing:border-box}
body{margin:0;font-family:'Segoe UI',system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--ink)}
header{position:sticky;top:0;z-index:50;background:linear-gradient(180deg,#16203a 0%,#131c31 100%);border-bottom:1px solid var(--line);padding:12px 22px;display:flex;flex-wrap:wrap;gap:12px;align-items:center}
header h1{font-size:17px;margin:0;display:flex;align-items:center;gap:10px}
header h1 .dot{width:12px;height:12px;border-radius:50%;background:var(--accent);box-shadow:0 0 12px var(--accent)}
.stats{display:flex;gap:8px;flex-wrap:wrap}
.stat{background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:4px 10px;font-size:12px;color:var(--muted)}
.stat b{color:var(--ink)}
.controls{margin-left:auto;display:flex;gap:8px;flex-wrap:wrap;align-items:center}
button,.sel{background:var(--panel2);color:var(--ink);border:1px solid var(--line);border-radius:8px;padding:7px 12px;font-size:13px;cursor:pointer}
button:hover,.sel:hover{border-color:var(--accent)}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
#search{background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:7px 12px;color:var(--ink);font-size:13px;width:230px}
#search:focus{outline:none;border-color:var(--accent)}
#status{position:fixed;left:14px;bottom:12px;z-index:40;background:rgba(24,33,54,.92);border:1px solid var(--line);
  border-radius:8px;padding:5px 10px;font-size:11px;color:var(--muted)}
#status b{color:var(--ink)}
#tip{position:fixed;z-index:60;display:none;max-width:430px;background:rgba(15,21,35,.97);border:1px solid var(--line);
  border-radius:10px;padding:10px 12px;font-size:12px;line-height:1.45;box-shadow:0 8px 24px rgba(0,0,0,.5)}
#tip .t{font-weight:700;font-size:13px;margin-bottom:2px}
#tip .p{color:var(--muted);font-family:ui-monospace,monospace;font-size:10.5px;word-break:break-all;margin-bottom:6px}
#tip .row{color:var(--ink)}
#tip .mut{color:var(--muted)}
#tip .crit{color:#fca5a5;margin-top:5px}
svg{display:block;width:100vw;height:calc(100vh - 62px);cursor:grab}
svg:active{cursor:grabbing}
.link{fill:none;stroke:#2b3a5e;stroke-width:1.1px}
.link.hot{stroke:#5b8def}
.node circle{cursor:pointer;stroke:#0f1523;stroke-width:1.2px}
.node text{font-size:10px;fill:#c9d6ee;paint-order:stroke;stroke:#0f1523;stroke-width:2.5px;pointer-events:none}
.node.more circle{fill:#334155;stroke:#94a3b8;stroke-dasharray:2 2}
.node.more text{fill:#94a3b8}
mark{background:#facc15;color:#111;border-radius:3px;padding:0 2px}
</style>
</head>
<body>
<header>
  <h1><span class="dot"></span>OmniCommerce Onion — Visual Review</h1>
  <div class="stats" id="stats"></div>
  <div class="controls">
    <input id="search" type="text" placeholder="Search name or path… (jumps + expands)">
    <select class="sel" id="depth" title="expand every visible branch down to this layer">
      <option value="0">Expand: phases</option>
      <option value="1">Expand: artifacts (L1)</option>
      <option value="2">Expand: tasks (L2)</option>
    </select>
    <select class="sel" id="branch">
      <option value="all">All artifacts</option>
      <option value="IN">Inputs only</option>
      <option value="OUT">Outputs only</option>
    </select>
    <button onclick="expandAll()">Expand all</button>
    <button onclick="expandBelow()" title="expand every node under the selected node">Expand below &#10515;</button>
    <button onclick="collapseBelow()" title="collapse the selected node's subtree">Collapse below &#10514;</button>
    <button onclick="collapseAll()">Collapse all</button>
    <button onclick="fit()">Fit</button>
    <button class="primary" onclick="locate()">Clear search</button>
  </div>
</header>
<svg id="svg"></svg>
<div id="status">loading…</div>
<div id="tip"></div>

<script>__D3__</script>
<script>
const DATA = __PAYLOAD__;
const LEVEL_COLORS = ['#8b5cf6','#10b981','#3b82f6','#06b6d4','#84cc16',
                      '#ec4899','#d946ef','#f43f5e','#fb923c','#eab308'];
const CHUNK = 2000;        // max children rendered per node before a "more" node
const LABEL_MAX = 300;     // draw text labels only under this many visible nodes
const SUBTREE_CAP = 20000; // max VISIBLE nodes revealed by one "expand below" action

const svg = d3.select('#svg');
const tip = document.getElementById('tip');
const statusEl = document.getElementById('status');
let gRoot = null, gLinks = null, gNodes = null, lastNodes = [], lastLinks = [];
let byPath = new Map();    // materialized path -> node ref
let highlight = null;      // path of the last search hit
let selectedPath = null;   // path of the last clicked node (for expand/collapse below)
let branchFilter = 'all';

/* ---------- raw data helpers ---------- */
function childKey(d){
  if (d.artifacts) return 'artifacts';
  if (d.tasks) return 'tasks';
  if (d.subtasks) return 'subtasks';
  if (d.details) return 'details';
  if (d.items) return 'items';
  return null;
}
function rawChildren(d){
  const k = childKey(d);
  if (!k) return [];
  let es = Object.entries(d[k]);
  if (branchFilter !== 'all' && k === 'artifacts'){
    es = es.filter(([p]) => {
      const seg = p.split('-')[1] || '';
      return branchFilter === 'IN' ? seg.startsWith('IN') : !seg.startsWith('IN');
    });
  }
  return es;
}

/* ---------- lazy materialization ---------- */
function mk(path, data){
  const n = {path, data, expanded:false, kids:null, cap:CHUNK};
  byPath.set(path, n);
  return n;
}
function materialize(n){
  const all = rawChildren(n.data);
  const take = Math.min(all.length, n.cap);
  const kids = all.slice(0, take).map(([p, d]) => byPath.get(p) || mk(p, d));
  if (all.length > take) kids.push({more:true, parent:n, left:all.length - take, path:n.path + '…more'});
  n.kids = kids;
}
function expand(n){
  if (n.more) return;
  n.expanded = true;
  if (!n.kids) materialize(n);
}
function collapse(n){ if (!n.more) n.expanded = false; }

const virtual = {virtual:true, data:{name:'Onion', level:-1}, expanded:true, kids:[],
                 path:'', cap:1e9};
for (const [pid, p] of Object.entries(DATA.tree)) virtual.kids.push(mk(pid, p));

/* ---------- layout + render ---------- */
function build(d){
  if (d.virtual) return {n:d, children:d.kids.map(build)};
  if (d.more) return {n:d};
  if (d.expanded && d.kids) return {n:d, children:d.kids.map(build)};
  return {n:d};
}
function layout(){
  const root = d3.hierarchy(build(virtual), d => d.children);
  d3.tree().nodeSize([16, 240])(root);   // [sibling spacing, depth spacing]
  return root;
}
function render(){
  hideTip();
  const root = layout();
  lastNodes = root.descendants();
  lastLinks = root.links();

  if (!gRoot){
    gRoot = svg.append('g');
    gLinks = gRoot.append('g').attr('class','links');
    gNodes = gRoot.append('g').attr('class','nodes');
    const zoom = d3.zoom().scaleExtent([0.05, 4])
      .on('zoom', e => gRoot.attr('transform', e.transform));
    svg.call(zoom);
    svg.on('dblclick.zoom', null);
    svg.on('dblclick', () => fit());
    window.__zoom = zoom;
  }

  const visible = lastNodes.length;
  const showLabels = visible <= LABEL_MAX;

  gLinks.selectAll('path').data(lastLinks, d => d.target.data.n.path)
    .join('path')
      .attr('class', d => 'link' + (highlight && (d.target.data.n.path === highlight ||
             d.source.data.n.path === highlight) ? ' hot' : ''))
      .attr('d', d3.linkHorizontal().x(d => d.y).y(d => d.x));

  const sel = gNodes.selectAll('g.node')
    .data(lastNodes.filter(d => !d.data.n.virtual), d => d.data.n.path)
    .join(enter => {
      const g = enter.append('g').attr('class','node');
      g.append('circle');
      g.append('path').attr('class','atomic');
      g.append('text');
      return g;
    })
    .attr('class', d => 'node' + (d.data.n.more ? ' more' : ''))
    .attr('transform', d => `translate(${d.y},${d.x})`);

  sel.select('circle')
    .attr('r', d => d.data.n.more ? 5 : (d.data.n.expanded ? 5.5 : 4.5))
    .attr('fill', d => d.data.n.more ? '#334155' : LEVEL_COLORS[d.data.n.data.level] || '#94a3b8')
    .attr('stroke',
      d => highlight && d.data.n.path === highlight ? '#facc15'
         : d.data.n.path === selectedPath ? '#38bdf8'
         : d.data.n.data.funnel ? '#f8fafc'
         : d.data.n.data.critique ? '#f87171'
         : '#0f1523')
    .attr('stroke-width', d => (highlight && d.data.n.path === highlight) ? 2.5
         : d.data.n.path === selectedPath ? 2.4
         : (d.data.n.data.funnel || d.data.n.data.critique) ? 1.8 : 1.2);

  sel.select('path.atomic')
    .attr('d', 'M0,-2.7 L2.3,0 L0,2.7 L-2.3,0 Z')
    .attr('fill', '#0f1523')
    .attr('stroke', '#f8fafc')
    .attr('stroke-width', 0.7)
    .style('display', d => (d.data.n.data && d.data.n.data.atomic && !d.data.n.more) ? '' : 'none');

  sel.select('text')
    .style('display', showLabels ? '' : 'none')
    .attr('x', 10)
    .attr('y', 3.5)
    .attr('text-anchor', 'start')
    .text(d => {
      const n = d.data.n;
      if (n.more) return `… ${n.left} more`;
      const t = (n.data.name || n.data.id || n.path);
      return t.length > 28 ? t.slice(0, 26) + '…' : t;
    });

  sel.on('click', (event, d) => {
    const n = d.data.n;
    event.stopPropagation();
    if (n.more){ n.parent.cap += CHUNK; materialize(n.parent); render(); return; }
    if (n.expanded) collapse(n); else expand(n);
    selectedPath = n.path;
    render();
  })
  .on('contextmenu', (event, d) => {
    event.preventDefault();
    const n = d.data.n;
    if (n.more) return;
    selectedPath = n.path;
    if (n.expanded){
      const cnt = collapseSubtree(n);
      render(); centerOn(n.path);
      statusEl.innerHTML = `collapsed <b>${cnt}</b> nodes under <b>${esc(n.path)}</b>`;
    } else {
      const r = expandSubtree(n);
      render(); centerOn(n.path);
      statusEl.innerHTML = `expanded <b>${r.count}</b> nodes under <b>${esc(n.path)}</b>` +
        (r.capped ? ` — capped at ${SUBTREE_CAP}; zoom into a branch to continue` : '');
    }
  })
  .on('mousemove', (event, d) => showTip(event, d.data.n))
  .on('mouseleave', hideTip);

  statusEl.innerHTML = `visible <b>${visible - 1}</b> nodes` +
    (root.height ? ` · depth <b>${root.height}</b>` : '') +
    (showLabels ? '' : ' · labels hidden (zoom in or collapse)');
  if (!window.__zoomed && visible > 1){ fit(); window.__zoomed = true; }
}

/* ---------- zoom / fit ---------- */
function fit(){
  if (!lastNodes.length) return;
  // horizontal tree: world X = depth (d.y), world Y = sibling order (d.x)
  const xs = lastNodes.map(d => d.y), ys = lastNodes.map(d => d.x);
  const x0 = Math.min(...xs), x1 = Math.max(...xs);
  const y0 = Math.min(...ys), y1 = Math.max(...ys);
  const W = window.innerWidth, H = window.innerHeight - 62;
  const k = Math.min(2, 0.92 * Math.min((W - 80) / Math.max(90, x1 - x0 + 160),
                                        (H - 60) / Math.max(60, y1 - y0 + 80)));
  const t = d3.zoomIdentity.translate(60 - k*x0, H/2 - k*(y0+y1)/2).scale(k);
  svg.call(window.__zoom.transform, t);
}
function centerOn(path){
  const d = lastNodes.find(d => d.data.n.path === path);
  if (!d) return;
  const W = window.innerWidth, H = window.innerHeight - 62;
  const k = Math.max(0.5, Math.min(1.2, (window.__zoom ? d3.zoomTransform(svg.node()).k : 1)));
  svg.call(window.__zoom.transform,
           d3.zoomIdentity.translate(W*0.25 - k*d.y, H/2 - k*d.x).scale(k));
}

/* ---------- tooltip ---------- */
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function showTip(ev, n){
  if (n.virtual){ hideTip(); return; }
  const d = n.data;
  let h = `<div class="t">${esc(d.name || d.id || n.path)}</div>`;
  h += `<div class="p">${esc(n.path)}</div>`;
  h += `<div class="row">level <b>L${d.level}</b> · children <b>${d.children != null ? d.children : '?'}</b>` +
       (d.atomic ? ` · <b style="color:#e2e8f0">atomic ◇</b>` : '') +
       (n.more ? ` · <span class="mut">${n.left} not rendered (click to load)</span>` : '') + `</div>`;
  if (d.funnel){
    h += `<div class="row">funnel — kept <b>${d.funnel.kept}</b> · atomic <b>${d.funnel.atomic}</b>` +
         ` · score <b>${d.funnel.score}</b> · retries ${d.funnel.retries}</div>`;
  }
  if (d.critique){
    const iss = (d.critique.issues || '').slice(0, 420);
    h += `<div class="crit">critique score ${d.critique.score}` +
         (iss ? `: ${esc(iss)}${d.critique.issues.length > 420 ? '…' : ''}` : '') + `</div>`;
  }
  tip.innerHTML = h;
  tip.style.display = 'block';
  const pad = 14;
  let x = ev.clientX + pad, y = ev.clientY + pad;
  if (x + tip.offsetWidth > innerWidth) x = ev.clientX - tip.offsetWidth - pad;
  if (y + tip.offsetHeight > innerHeight) y = ev.clientY - tip.offsetHeight - pad;
  tip.style.left = x + 'px'; tip.style.top = y + 'px';
}
function hideTip(){ tip.style.display = 'none'; }

/* ---------- subtree expand / collapse (selected node) ---------- */
function expandSubtree(root, cap = SUBTREE_CAP) {
  let visible = 0, capped = false;
  const q = [root];
  while (q.length) {
    const x = q.shift();
    if (x.more) continue;
    if (!x.expanded) expand(x);
    if (x.kids) {
      visible += x.kids.length;
      for (const k of x.kids) if (!k.more) q.push(k);
    }
    if (visible >= cap) { capped = q.length > 0; break; }
  }
  return {count: visible, capped};
}
function collapseSubtree(root) {
  let count = 0;
  const q = [root];
  while (q.length) {
    const x = q.shift();
    if (x.more) continue;
    if (x.expanded) { collapse(x); count++; }
    if (x.kids) for (const k of x.kids) if (!k.more) q.push(k);
  }
  return count;
}
function expandBelow() {
  if (!selectedPath) { statusEl.innerHTML = 'click a node first, then Expand below'; return; }
  const n = byPath.get(selectedPath);
  if (!n) { statusEl.innerHTML = 'selected node is no longer materialized'; return; }
  const t0 = performance.now();
  const r = expandSubtree(n);
  render();
  centerOn(selectedPath);
  statusEl.innerHTML = `expanded <b>${r.count}</b> nodes under <b>${esc(selectedPath)}</b>` +
    (r.capped ? ` — capped at ${SUBTREE_CAP}; zoom into a branch to continue` : '') +
    ` · ${Math.round(performance.now() - t0)} ms`;
}
function collapseBelow() {
  if (!selectedPath) { statusEl.innerHTML = 'click a node first, then Collapse below'; return; }
  const n = byPath.get(selectedPath);
  if (!n) { statusEl.innerHTML = 'selected node is no longer materialized'; return; }
  const cnt = collapseSubtree(n);
  render();
  centerOn(selectedPath);
  statusEl.innerHTML = `collapsed <b>${cnt}</b> nodes under <b>${esc(selectedPath)}</b>`;
}

/* ---------- expand all / collapse ---------- */
function expandAll(){
  const MAXV = 20000;   // cap on VISIBLE nodes (full tree is 300k+; SVG chokes)
  let visibleN = 0, expandedN = 0, capped = false;
  const q = virtual.kids.slice();
  while (q.length){
    const n = q.shift();
    if (n.more) continue;
    if (!n.expanded) expand(n);
    expandedN++;
    if (n.kids){
      visibleN += n.kids.length;
      for (const k of n.kids){ if (!k.more) q.push(k); }
    }
    if (visibleN >= MAXV){ capped = q.length > 0; break; }
  }
  render();
  fit();
  statusEl.innerHTML = capped
    ? `expand-all capped at ~<b>${MAXV}</b> visible nodes of ${DATA.summary.total_nodes} — zoom into a branch to continue`
    : `expanded <b>${expandedN}</b> nodes`;
}
function collapseAll(){
  const clear = n => { n.expanded = false; if (n.kids) n.kids.forEach(k => { if (!k.more) clear(k); }); };
  virtual.kids.forEach(clear);
  highlight = null;
  render();
  fit();
}
function expandTo(depth){
  let guard = 0;
  const step = n => {
    if (n.more || guard++ > 50000) return;
    if (n.data.level < depth){
      expand(n);
      (n.kids || []).forEach(k => step(k));
    }
  };
  virtual.kids.forEach(step);
  render();
  fit();
}

/* ---------- search ---------- */
function walkRaw(d, path, q, out){
  if (out.length >= 100) return;
  const nm = (d.name || '').toLowerCase();
  if (nm.includes(q) || path.toLowerCase().includes(q)) out.push(path);
  const k = childKey(d);
  if (k) for (const [p, c] of Object.entries(d[k])) walkRaw(c, p, q, out);
}
function resolve(path){
  const segs = path.split('-');
  let n = byPath.get(segs[0]);
  if (!n || !DATA.tree[segs[0]]) return null;
  expand(n);
  let cur = segs[0];
  for (let i = 1; i < segs.length; i++){
    cur += '-' + segs[i];
    let child = byPath.get(cur);
    if (!child){
      const k = childKey(n.data);
      const entry = k ? Object.entries(n.data[k]).find(([p]) => p === cur) : null;
      if (!entry) return null;
      child = mk(cur, entry[1]);
      if (!n.kids) expand(n);
      n.kids.unshift(child);
    }
    expand(child);
    n = child;
  }
  return n;
}
function locate(){ document.getElementById('search').value = ''; highlight = null; render(); }
document.getElementById('search').addEventListener('keydown', ev => {
  if (ev.key !== 'Enter') return;
  const q = (ev.target.value || '').trim().toLowerCase();
  if (!q){ highlight = null; render(); return; }
  const out = [];
  for (const [pid, p] of Object.entries(DATA.tree)){ walkRaw(p, pid, q, out); if (out.length >= 100) break; }
  if (!out.length){ statusEl.innerHTML = `no match for <b>${esc(q)}</b>`; return; }
  highlight = out[0];
  const n = resolve(highlight);
  render();
  if (n) centerOn(highlight);
  statusEl.innerHTML = `<b>${out.length}${out.length >= 100 ? '+' : ''}</b> matches · showing first: <b>${esc(highlight)}</b>`;
});
document.getElementById('depth').addEventListener('change', ev => {
  const d = +ev.target.value; ev.target.value = '0';
  if (d > 0) expandTo(d); else collapseAll();
});
document.getElementById('branch').addEventListener('change', ev => {
  branchFilter = ev.target.value;
  byPath = new Map();
  virtual.kids = Object.entries(DATA.tree).map(([pid, p]) => mk(pid, p));
  highlight = null;
  render();
  fit();
});

/* ---------- header stats ---------- */
function stats(){
  const el = document.getElementById('stats');
  const s = [];
  for (const k of Object.keys(DATA.levels).sort()){
    s.push(`<span class="stat">L${+k.slice(-1)} <b>${DATA.levels[k].count}</b></span>`);
  }
  const sm = DATA.summary || {};
  if (DATA.json_size)  s.push(`<span class="stat">json <b>${DATA.json_size}</b></span>`);
  if (sm.total_nodes != null) s.push(`<span class="stat">nodes <b>${sm.total_nodes}</b></span>`);
  if (sm.funneled != null)   s.push(`<span class="stat">funneled <b>${sm.funneled}</b></span>`);
  if (sm.kept_total != null) s.push(`<span class="stat">kept <b>${sm.kept_total}</b></span>`);
  if (sm.atomic_total != null) s.push(`<span class="stat">atomic <b>${sm.atomic_total}</b></span>`);
  el.innerHTML = s.join('');
}
stats();
render();
</script>
</body>
</html>"""

html = HTML.replace("__PAYLOAD__", payload)
if d3_js:
    html = html.replace("__D3__", d3_js)
else:
    html = html.replace('<script>__D3__</script>',
                        '<script src="https://cdn.jsdelivr.net/npm/d3@7/dist/d3.min.js"></script>')
out = os.path.join(ROOT, "Onion_Review.html")
with open(out, "w", encoding="utf-8") as f:
    f.write(html)
print("wrote", out, os.path.getsize(out) // 1024, "KB")
