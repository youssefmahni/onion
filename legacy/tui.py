#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tui.py — Onion Mission Control, terminal edition (Textual).

Consumes the exact same /api/state JSON the web dashboard uses; if the
controller isn't running it spawns one first.

    .venv/bin/python tui.py                    # auto-spawns the controller
    .venv/bin/python tui.py --no-spawn         # require a running controller
    .venv/bin/python tui.py --seed P01-IN03-T30 --limit 10
    .venv/bin/python tui.py --port 8090            # controller on a custom port

Keys: q quit · p pause · s start/continue · t stop · +/- zoom · 0-9 select layer · c clear
"""
import argparse, json, math, os, subprocess, sys, time
from urllib import request as urlreq

try:
    from textual.app import App, ComposeResult
    from textual.containers import Horizontal, Vertical
    from textual.widgets import Button, Footer, Header, Label, ProgressBar, Select, Static
    from textual import events
    from rich.text import Text
except ImportError:
    print("missing deps — run:  python3 -m venv .venv && .venv/bin/pip install textual")
    sys.exit(1)

ROOT = os.path.dirname(os.path.abspath(__file__))
STATE = os.environ.get("ONION_TUI_URL", "http://127.0.0.1:8080/api/state")

LAYER_NAMES = ["Phases","Inputs & Outputs","Tasks","Sub-tasks","Task Details","Items","Sub-items","L7","L8","L9"]
LAYER_DESC = [
  "Program phases — the top-level roadmap.",
  "Artifacts — the inputs consumed and outputs produced by the work.",
  "Tasks — the blocks of work inside an artifact.",
  "Sub-tasks — sequenced steps within a task.",
  "Task details — verb-level activities (map, measure, document…).",
  "Items — the knowledge payload: the things an agent uses.",
  "Sub-items — nested decompositions of items.",
  "Layer 7 — deeper nesting (reserved).",
  "Layer 8 — deeper nesting (reserved).",
  "Layer 9 — deepest nesting level (hard cap)."]
LAYER_COLORS = ["#8b5cf6","#10b981","#3b82f6","#06b6d4","#84cc16",
                "#ec4899","#d946ef","#f43f5e","#fb923c","#eab308"]

def policy_target(cores, load1, ram, net, link, gpu, prev):
    base = min(16, max(2, cores))
    t = min(5000, max(1, int(prev)))
    if load1 > 0.8*cores: t -= 2
    elif load1 < 0.3*cores: t += 2
    if t > base and load1 < 0.3*cores: t = base
    if ram < 1.0: t = t // 2
    if link and link > 0 and net > 0.7*link: t = min(t, 4)
    if gpu and gpu > 90: t = min(t, 2)
    return min(5000, max(1, t))

SPARK = "▁▂▃▄▅▆▇█"

def sparkline(vals, width, color):
    t = Text()
    if not vals:
        t.append("collecting…", style="#5f7196")
        return t
    lo, hi = min(vals), max(vals)
    rng = (hi - lo) or 1.0
    step = max(1, len(vals) // max(1, width))
    sampled = vals[::step][-width:]
    for v in sampled:
        idx = int((v - lo) / rng * (len(SPARK) - 1))
        t.append(SPARK[max(0, min(len(SPARK) - 1, idx))], style=color)
    return t


def api(method="GET", path="", body=None):
    req = urlreq.Request(STATE.replace("/api/state", path) if path else STATE, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
        req.data = json.dumps(body).encode()
    try:
        with urlreq.urlopen(req, timeout=3) as r:
            return json.loads(r.read() or b"{}")
    except Exception:
        return None


# ---------------- rings widget (braille-ellipse onion) ----------------
BRAILLE = [
    [0x01, 0x08], [0x02, 0x10], [0x04, 0x20], [0x40, 0x80],   # rows 0-3, cols 0-1
]

class Rings(Static):
    """Braille rings with exactly one dot per node in the layer; click to select."""
    def __init__(self, id=None):
        super().__init__("", id=id)
        self.state = None
        self.selected = None
        self.zoom = 1.0

    def aspect(self, W, H):
        # stretch the onion into a wide ellipse so it fills a 50/50 panel
        return max(1.0, min(2.4, (W * 2) / max(1, H * 4)))

    def ring_bands(self, W=None, H=None):
        # dot-space: cell = 2 dots wide, 4 dots tall
        W = W or (self.size.width if self.size else 40)
        H = H or (self.size.height if self.size else 14)
        max_r = 8 + 9 * 4.2 + 2.6        # outermost edge at zoom 1
        avail = (H * 4 / 2) - 2          # fill the height; ellipse handles width
        fit = max(0.5, avail / max_r)
        scale = fit * self.zoom
        return [((8 + i*4.2) * scale, (8 + i*4.2 + 2.6) * scale) for i in range(10)]

    def set_state(self, st, selected):
        self.state = st
        self.selected = selected

    def render_rings(self):
        t = Text(no_wrap=True)
        W = self.size.width if self.size else 40
        H = self.size.height if self.size else 14
        if W <= 0 or H <= 0:
            return t
        bands = self.ring_bands(W, H)
        asp = self.aspect(W, H)
        cx, cy = W * 2 / 2, H * 4 / 2          # center in dots (2x4 per cell)
        active = None
        if self.state:
            a = (self.state.get("funnel") or {}).get("active_layer")
            alive = (self.state.get("process") or {}).get("alive")
            if alive and a:
                active = int(a[1:])
        counts = (self.state or {}).get("layers") or {}

        grid = [[0] * W for _ in range(H)]     # braille bit mask per cell
        owner = [[-1] * W for _ in range(H)]   # ring id per cell (last wins)
        for i in range(10):
            n = int(counts.get(f"L{i}", 0) or 0)
            if n <= 0:
                continue
            rmid = (bands[i][0] + bands[i][1]) / 2.0
            step = (2.0 * math.pi) / n
            ca, sa = math.cos(step), math.sin(step)
            c, s = 1.0, 0.0                     # incremental rotation, no per-dot trig
            for _ in range(n):
                sx = cx + c * rmid * asp
                sy = cy + s * rmid
                col = int(sx) // 2
                row = int(sy) // 4
                if 0 <= col < W and 0 <= row < H:
                    grid[row][col] |= BRAILLE[int(sy) % 4][int(sx) % 2]
                    owner[row][col] = i
                c, s = c * ca - s * sa, s * ca + c * sa

        for row in range(H):
            for col in range(W):
                bits = grid[row][col]
                if bits:
                    ring = owner[row][col]
                    style = LAYER_COLORS[ring] if ring >= 0 else ""
                    if ring == self.selected:
                        style += " bold reverse"
                    elif ring == active:
                        style += " bold"
                    t.append(chr(0x2800 + bits), style=style)
                else:
                    t.append(" ")
            if row < H - 1:
                t.append("\n")
        return t

    def on_click(self, event: events.Click):
        if not self.state or not self.size:
            return
        x, y = event.offset.x, event.offset.y
        bands = self.ring_bands()
        cx, cy = self.size.width * 2 / 2, self.size.height * 4 / 2
        asp = self.aspect(self.size.width, self.size.height)
        dx = (x*2 + 1 - cx) / asp
        dy = y*4 + 2 - cy
        d = (dx*dx + dy*dy) ** 0.5
        hit = None
        for i, (ri, ro) in enumerate(bands):
            if ri - 0.8 <= d <= ro + 0.8:
                hit = i
        self.app.select_layer(hit)


# ---------------- app ----------------
class OnionTui(App):
    CSS = """
    Screen { background: #05080f; }
    #gauges { height: 3; border: round #22d3ee; background: #0a1428; }
    #left { width: 1fr; border: round #22d3ee; background: #081226; }
    #right { width: 1fr; border: round #22d3ee; background: #05080f; }
    .card { border: solid #22304a; padding: 0 1; margin-bottom: 1; }
    .card-title { color: #22d3ee; text-style: bold; }
    #rings { height: 1fr; content-align: center middle; }
    #layerCard { height: 7; border: solid #22304a; padding: 0 1; }
    #keys { height: 1; color: #5f7196; }
    #stateLine { height: 1; margin-bottom: 1; text-style: bold; }
    #btnRow { height: 3; }
    .actbtn { min-width: 10; margin-right: 1; }
    #capLine { height: 3; align: left middle; }
    #capLabelTxt { width: auto; padding: 1 2 0 0; color: #8fa0bf; }
    #capSelect { width: 30; }
    Button { margin-right: 1; }
    #fBar { width: 100%; }
    .good { color: #86efac; }
    .warn { color: #fbbf24; }
    .bad  { color: #fda4af; }
    .mut  { color: #8fa0bf; }
    """
    BINDINGS = [
        ("q", "quit", "quit"),
        ("p", "pause", "pause"),
        ("s", "start", "start/continue"),
        ("t", "stop", "stop"),
        ("+", "zoom_in", "zoom in"),
        ("-", "zoom_out", "zoom out"),
        ("c", "clear", "deselect"),
    ] + [(str(i), f"pick_{i}", f"select L{i}") for i in range(10)]

    def __init__(self, args):
        super().__init__()
        self.args = args
        self.state = None
        self.selected = None

    def compose(self) -> ComposeResult:
        yield Static("", id="gauges")
        with Horizontal():
            with Vertical(id="left"):
                yield Static("Machine Load", classes="card-title")
                with Vertical(classes="card"):
                    yield Static("", id="loadCard")
                    yield Static("", id="spark")
                yield Static("Funnel — live run", classes="card-title")
                with Vertical(classes="card"):
                    yield Static("", id="funnelCard")
                    yield ProgressBar(total=1, show_eta=False, id="fBar")
                    yield Static("", id="fbarTxt", classes="mut")
                yield Static("Nodes — agent progress", classes="card-title")
                yield Static("", id="nodesCard")
                yield Static("Control", classes="card-title")
                with Vertical(classes="card"):
                    yield Static("", id="stateLine")
                    with Horizontal(id="btnRow"):
                        yield Button("Pause", id="btnPause", variant="warning", classes="actbtn")
                        yield Button("Stop", id="btnStop", variant="error", classes="actbtn")
                        yield Button("Continue", id="btnCont", variant="success", classes="actbtn")
                        yield Button("Start", id="btnStart", variant="success", classes="actbtn")
                    with Horizontal(id="capLine"):
                        yield Label("thread cap", id="capLabelTxt")
                        yield Select(
                            [("Auto (policy)", 0), ("1", 1), ("4", 4), ("8", 8),
                             ("16", 16), ("32", 32), ("64", 64), ("100", 100),
                             ("150", 150), ("250", 250), ("500", 500), ("1000", 1000),
                             ("2000", 2000), ("5000", 5000)],
                            value=0, allow_blank=False, id="capSelect")
            with Vertical(id="right"):
                yield Rings(id="rings")
                yield Static("", id="layerCard")
                yield Static("q quit · p pause · s start · t stop · +/- zoom · 0-9 layer · click ring", id="keys")

    def on_mount(self):
        self.set_interval(2, self.tick)
        self.poll(initial=True)
        if self.state:
            self.call_after_refresh(self.render_all)

    def on_resize(self, event):
        if self.state:
            self.call_after_refresh(self.render_all)

    def poll(self, initial=False):
        st = api()
        if st is None and initial and not self.args.no_spawn:
            self.spawn_controller()
            return
        if st is None:
            self.query_one("#gauges", Static).update(
                Text("no controller — waiting…", style="yellow"))
            return
        self.state = st
        self.render_all()

    def spawn_controller(self):
        cmd = ["python3", "controller.py"]
        if self.args.seed:
            cmd += ["--seed", self.args.seed]
        if self.args.limit:
            cmd += ["--limit", str(self.args.limit)]
        if getattr(self.args, "port", 8080) != 8080:
            cmd += ["--port", str(self.args.port)]
        subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def tick(self):
        self.poll()

    def render_all(self):
        st = self.state
        g = st.get("gauges") or {}
        f = st.get("funnel") or {}
        run = st.get("run") or {}
        pr = st.get("progress") or {}
        alive = bool((st.get("process") or {}).get("alive"))
        flags = st.get("flags") or {}

        # gauges
        rec = policy_target(g.get("cores", 8), g.get("load1", 0), g.get("ram_avail_gb", 8),
                            (g.get("net_rx_mbps", 0) + g.get("net_tx_mbps", 0)),
                            g.get("link_mbps"), g.get("gpu_pct", -1) or -1, st.get("target_threads", 8))
        t = Text()
        t.append(f" LOAD {g.get('load1', 0):.2f} ", style="cyan")
        t.append(f"CPU {round(g.get('cpu_busy', 0))}% ", style="magenta")
        t.append(f"RAM {g.get('ram_avail_gb', 0):.1f}GB ", style="green")
        t.append(f"NET {round(g.get('net_rx_mbps', 0) + g.get('net_tx_mbps', 0))} ", style="yellow")
        t.append(f"GPU {g.get('gpu_pct')} ", style="yellow")
        t.append(f"THREADS {st.get('target_threads', '?')}/{f.get('live_threads', '?')} ", style="blue bold")
        t.append(f"pid {(st.get('process') or {}).get('pid') or '—'} ", style="#8fa0bf")
        t.append(f"policy→{rec} ", style="cyan")
        status = "RUNNING" if alive else ("PAUSED" if flags.get("paused") else ("STOPPED" if flags.get("stopped") else "IDLE"))
        t.append(status, style="green bold" if alive else ("yellow bold" if flags.get("paused") else "red bold"))
        self.query_one("#gauges", Static).update(t)

        # machine load sparkline (native block characters)
        hist = st.get("history") or []
        spark_widget = self.query_one("#spark", Static)
        W = max(20, (spark_widget.size.width or (self.size.width // 2)) - 5)
        sp = Text()
        if not hist:
            sp.append("collecting…", style="#5f7196")
        else:
            for lbl, key, col in (("load", "load", "cyan"), ("cpu", "cpu", "magenta"),
                                  ("ram", "ram", "green"), ("net", "net", "yellow"),
                                  ("thr", "threads", "blue")):
                sp.append(f"{lbl:>4} ", style="#8fa0bf")
                sp.append_text(sparkline([h.get(key, 0) for h in hist], W, col))
                sp.append("\n")
        self.query_one("#spark", Static).update(sp)
        self.query_one("#loadCard", Static).update(
            f"load1 {g.get('load1', 0):.2f}  load5 {g.get('load5', 0):.2f}  cores {g.get('cores', '?')}")

        # funnel
        ft = Text()
        ft.append(f"root {run.get('seed', '—')}\n", style="#8fa0bf")
        ft.append(f"node {f.get('node_title') or '—'}\n")
        stage = f.get("stage")
        ft.append(f"step {stage['agent']} {stage['ev']}" if stage else "step —", style="cyan")
        ft.append(f"\nloop {f.get('done', 0)}/{run.get('limit', '∞')}   queued {pr.get('queued', '–')}   "
                  f"fails {f.get('fail', 0)}")
        agents = pr.get("agents") or {}
        agl = {"planner": "P", "worker": "W", "trimmer": "T", "critique": "C", "synthesizer": "S"}
        ft.append("\nagents ", style="#8fa0bf")
        for k, lbl in agl.items():
            a = agents.get(k) or {}
            ft.append(f"{lbl}{a.get('done', 0)}", style="cyan")
            if a.get("running"):
                ft.append(f"+{a['running']}", style="yellow")
            ft.append(" ")
        sm = f.get("summary") or ""
        if sm:
            import re as _re
            def gv(k):
                m = _re.search(k + r"=([0-9]+)", sm)
                return m.group(1) if m else "–"
            ft.append("\n", style="")
            for k, lbl in (("funneled", "fun"), ("kept", "kept"), ("atomic", "atom"),
                           ("calls", "calls"), ("retried", "ret"), ("errors", "err")):
                ft.append(f"{lbl} {gv(k)}  ", style="magenta")
        ft.append(f"\nlast: {f.get('last_line') or '—'}", style="#5f7196")
        self.query_one("#funnelCard", Static).update(ft)

        budget = run.get("limit") or 0
        final = pr.get("final", 0)
        pct = min(100, round(final*100/budget)) if budget else 0
        bar = self.query_one("#fBar", ProgressBar)
        bar.update(total=max(1, budget or 1), progress=min(final, budget or final) if budget else 0)
        self.query_one("#fbarTxt", Static).update(
            f"completion {pct}% — final {final} · retry {pr.get('retry', 0)} · budget {budget or '∞'}")

        # nodes
        nt = Text()
        for n in (pr.get("nodes") or [])[-9:][::-1]:
            s = n.get("status", "running")
            col = {"final": "green", "retry": "yellow", "failed": "red"}.get(s, "blue")
            stages = "".join({"done": "✓", "active": "●", "fail": "✗"}.get(n.get(k, "pending"), "○")
                              for k in ("P", "T", "C", "S"))
            w = f"W{n.get('W_done',0)}/{n.get('W_total',0)}" if n.get("W_total") else "W–"
            nt.append(f"{n.get('title') or n.get('path')}\n", style=col)
            nt.append(f"  {stages} {w}  score {n.get('score') or '–'}  kept {n.get('kept') or '–'}\n", style="#8fa0bf")
        if not (pr.get("nodes") or []):
            nt.append("no nodes in this run", style="#8fa0bf")
        self.query_one("#nodesCard", Static).update(nt)

        # controls — state chip styled by status
        status_col = {"RUNNING": "green", "PAUSED": "yellow", "STOPPED": "red"}.get(status, "grey50")
        chip = {"RUNNING": "● RUNNING", "PAUSED": "⏸ PAUSED", "STOPPED": "■ STOPPED"}.get(status, "○ IDLE — finished")
        self.query_one("#stateLine", Static).update(Text(chip, style=f"{status_col} bold"))
        self.query_one("#btnPause", Button).display = alive
        self.query_one("#btnStop", Button).display = alive or flags.get("paused")
        self.query_one("#btnCont", Button).display = bool(flags.get("paused"))
        self.query_one("#btnStart", Button).display = (not alive) and (not flags.get("paused"))
        # thread cap select: sync without firing a change request
        sel = self.query_one("#capSelect", Select)
        want = int(st.get("override") or 0)
        if sel.value != want:
            with self.prevent(Select.Changed):
                sel.value = want

        # rings + layer card
        rings = self.query_one("#rings", Rings)
        rings.set_state(st, self.selected)
        rings.update(rings.render_rings())
        if self.selected is not None:
            i = self.selected
            lt = Text()
            lt.append(f"L{i} — {LAYER_NAMES[i]}\n", style=f"{LAYER_COLORS[i]} bold")
            lt.append(f"nodes {st.get('layers', {}).get(f'L{i}', '…')}  depth {i}/9\n", style="#8fa0bf")
            lt.append(f"{LAYER_DESC[i]}\n", style="#8fa0bf")
            a = f.get("active_layer")
            if alive and a == f"L{i}":
                lt.append("BEING WRITTEN NOW", style="cyan reverse")
            self.query_one("#layerCard", Static).update(lt)
        else:
            self.query_one("#layerCard", Static).update(
                Text("onion legend — click a ring (mouse) or press 0-9", style="#22d3ee bold"))

    def select_layer(self, i):
        self.selected = None if i is None else i
        if self.state:
            self.render_all()

    # ---- actions ----
    def action_pause(self): api("POST", "/api/pause", {})
    def action_start(self): api("POST", "/api/resume", {})
    def action_stop(self): api("POST", "/api/stop", {})
    def action_zoom_in(self):
        r = self.query_one("#rings", Rings); r.zoom = min(3.0, r.zoom + 0.2)
        if self.state: self.render_all()
    def action_zoom_out(self):
        r = self.query_one("#rings", Rings); r.zoom = max(0.5, r.zoom - 0.2)
        if self.state: self.render_all()
    def action_clear(self): self.select_layer(None)

    def on_select_changed(self, event: Select.Changed):
        if event.select.id == "capSelect":
            api("POST", "/api/threads", {"max": int(event.value or 0)})

    def on_button_pressed(self, event: Button.Pressed):
        btn = event.button.id
        if btn == "btnPause": self.action_pause()
        elif btn == "btnStop": self.action_stop()
        elif btn == "btnCont": self.action_start()
        elif btn == "btnStart": self.action_start()



for _i in range(10):
    def _make(i):
        def _act(self):
            self.select_layer(i)
        return _act
    setattr(OnionTui, f"action_pick_{_i}", _make(_i))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-spawn", action="store_true")
    ap.add_argument("--seed", default=None)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--port", type=int, default=8080,
                    help="controller port to poll (default 8080)")
    ap.add_argument("--url", default=None,
                    help="full /api/state URL (overrides --port and ONION_TUI_URL)")
    args = ap.parse_args()
    global STATE
    if args.url:
        STATE = args.url
    elif args.port != 8080:
        STATE = f"http://127.0.0.1:{args.port}/api/state"
    OnionTui(args).run()


if __name__ == "__main__":
    main()
