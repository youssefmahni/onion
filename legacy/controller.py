#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
controller.py — machine supervisor for the Onion funnel.

Measures hardware resources (CPU, RAM, NIC, GPU-if-present) every few seconds,
computes a suitable worker-thread count with a hysteresis policy, writes it to
control/concurrency.txt (the C++ funnel live-resizes its pool), owns the funnel
process lifecycle (start/stop/restart-on-crash, pause/resume), publishes
state.json for the dashboards, periodically rebuilds Onion.json and
Onion_Review.html, and serves the project over HTTP so the WASM/WebGL
dashboard can load.

Usage:
  python3 controller.py --seed P01-IN01-T01-ST01 --limit 50
  python3 controller.py --seed P01-IN03-T30 --limit 10 --port 8080
  python3 controller.py --no-server --dry
"""
import argparse, json, os, re, signal, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))


class Sampler:
    def __init__(self):
        self.cores = os.cpu_count() or 1
        self.prev_stat = None
        self.prev_net = None
        self.gpu = None
        self.gpu_top = None
        self.link_mbps = None
        if os.path.exists("/sys/class/net"):
            try:
                for n in os.listdir("/sys/class/net"):
                    if n.startswith(("e", "w", "en", "wl")):
                        sp = f"/sys/class/net/{n}/speed"
                        if os.path.exists(sp):
                            v = int(open(sp).read().strip())
                            self.link_mbps = v if v > 0 else None
                        break
            except OSError:
                pass

    def _cpu_busy(self):
        try:
            parts = open("/proc/stat").readline().split()[1:]
            vals = [int(x) for x in parts[:8]]
            idle = vals[3] + vals[4]
            total = sum(vals)
            if self.prev_stat:
                pt, pi = self.prev_stat
                d_total = total - pt
                d_idle = idle - pi
                if d_total > 0:
                    return 100.0 * (1.0 - d_idle / d_total)
            self.prev_stat = (total, idle)
            return 0.0
        except OSError:
            return 0.0

    def _net_mbps(self):
        try:
            rx = tx = 0
            for line in open("/proc/net/dev").readlines()[2:]:
                if ":" in line:
                    iface = line.split(":")[0].strip()
                    if iface == "lo":
                        continue
                    f = line.split(":")[1].split()
                    rx += int(f[0]); tx += int(f[8])
            now = time.time()
            if self.prev_net:
                prx, ptx, pt = self.prev_net
                dt = now - pt
                if dt > 0:
                    return (rx - prx) * 8 / dt / 1e6, (tx - ptx) * 8 / dt / 1e6
            self.prev_net = (rx, tx, now)
            return 0.0, 0.0
        except (OSError, ValueError, IndexError):
            return 0.0, 0.0

    def _ram_avail_gb(self):
        try:
            for line in open("/proc/meminfo"):
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 1024 / 1024  # kB -> GB
        except OSError:
            pass
        return 99.0

    def _load(self):
        try:
            l = open("/proc/loadavg").read().split()
            return float(l[0]), float(l[1])
        except (OSError, ValueError):
            return 0.0, 0.0

    def _gpu_pct(self):
        """GPU busy % from the best available source, or None.
        Order: nvidia-smi -> intel_gpu_top (one JSON sample) -> Intel i915
        sysfs frequency ratio (cur/max; idle ~= 300/1300 = 23%, ramps to
        ~100% under load, which is what the >90% policy cap expects)."""
        pct = self._gpu_pct_nvidia()
        if pct is not None:
            return pct
        pct = self._gpu_pct_intel_top()
        if pct is not None:
            return pct
        pct = self._gpu_pct_i915_sysfs()
        if pct is not None:
            return pct
        return self._gpu_pct_ioreg()

    def _gpu_pct_nvidia(self):
        try:
            if self.gpu is False:
                return None
            if self.gpu is None:
                if subprocess.run(["which", "nvidia-smi"],
                                  capture_output=True).returncode != 0:
                    self.gpu = False
                    return None
                self.gpu = True
            out = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu",
                                  "--format=csv,noheader,nounits"],
                                 capture_output=True, text=True, timeout=5)
            return float(out.stdout.strip().splitlines()[0])
        except Exception:
            self.gpu = False
            return None

    def _gpu_pct_intel_top(self):
        if self.gpu_top is False:
            return None
        if self.gpu_top is None:
            if subprocess.run(["which", "intel_gpu_top"],
                              capture_output=True).returncode != 0:
                self.gpu_top = False
                return None
            self.gpu_top = True
        try:
            out = subprocess.run(["intel_gpu_top", "-J", "-s", "250"],
                                 capture_output=True, text=True, timeout=3)
            # intel_gpu_top streams comma-separated JSON objects; take the
            # first complete one
            text = out.stdout.strip()
            if not text:
                return None
            start = text.find("{")
            end = text.find("}", start)
            if start < 0 or end < 0:
                return None
            sample = json.loads(text[start:end + 1])
            engines = sample.get("engines") or {}
            busy = [float(e.get("busy", 0)) for e in engines.values()
                    if isinstance(e, dict)]
            return max(busy) if busy else None
        except Exception:
            return None

    def _gpu_pct_ioreg(self):
        """macOS: AMD/Intel GPUs expose utilization in ioreg (no sudo)."""
        try:
            out = subprocess.run(["ioreg", "-r", "-d", "1", "-c", "IOAccelerator"],
                                 capture_output=True, text=True, timeout=5)
            vals = re.findall(r'"Device Utilization %"=(\d+)', out.stdout)
            if not vals:
                vals = re.findall(r'"GPU Activity\(%\)"=(\d+)', out.stdout)
            return float(max(int(v) for v in vals)) if vals else None
        except Exception:
            return None

    def _gpu_pct_i915_sysfs(self):
        try:
            base = "/sys/class/drm/card0"
            cur = int(open(f"{base}/gt_cur_freq_mhz").read())
            mx = 0
            for f in ("gt_max_freq_mhz", "gt_boost_freq_mhz"):
                try:
                    mx = max(mx, int(open(f"{base}/{f}").read()))
                except OSError:
                    pass
            if mx <= 0:
                return None
            return round(100.0 * cur / mx, 1)
        except OSError:
            return None

    def sample(self):
        load1, load5 = self._load()
        rx, tx = self._net_mbps()
        return {
            "ts": time.time(),
            "cores": self.cores,
            "load1": load1,
            "load5": load5,
            "cpu_busy": self._cpu_busy(),
            "ram_avail_gb": self._ram_avail_gb(),
            "net_rx_mbps": rx,
            "net_tx_mbps": tx,
            "link_mbps": self.link_mbps,
            "gpu_pct": self._gpu_pct(),
        }


def compute_target(s, prev):
    """Hysteresis policy: grow when idle, shrink when the machine is busy."""
    cores = s["cores"]
    base = min(16, max(2, cores))
    t = prev
    if s["load1"] > 0.8 * cores:
        t = max(1, t - 2)          # user busy -> back off
    elif s["load1"] < 0.3 * cores:
        t = min(base, t + 2)       # idle -> use more workers
    if s["ram_avail_gb"] < 1.0:
        t = max(1, t // 2)         # memory pressure
    link = s.get("link_mbps")
    if link and (s["net_rx_mbps"] + s["net_tx_mbps"]) > 0.7 * link:
        t = min(t, 4)              # NIC saturated (API-bound) -> cap
    if s.get("gpu_pct") is not None and s["gpu_pct"] > 90:
        t = min(t, 2)              # GPU busy (e.g. user rendering)
    return max(1, min(MAX_THREADS, t))


MAX_THREADS = 5000

LAYER_CACHE = {}  # (fn, mtime, size) -> row count

def layer_counts(root):
    out = {}
    for lv in range(10):
        fn = os.path.join(root, f"Onion - level_{lv:02d}.csv")
        try:
            stt = os.stat(fn)
            key = (fn, stt.st_mtime, stt.st_size)
            if key not in LAYER_CACHE:
                with open(fn, encoding="utf-8", errors="replace") as f:
                    LAYER_CACHE[key] = max(0, sum(1 for _ in f) - 1)
            out[f"L{lv}"] = LAYER_CACHE[key]
        except OSError:
            out[f"L{lv}"] = 0
    return out


def read_funnel_log(root, max_bytes=8 * 1024 * 1024):
    """Read funnel_run.log, capped at max_bytes from the end (safety cap;
    the controller rotates the log per run so this rarely matters)."""
    fn = os.path.join(root, "funnel_run.log")
    try:
        sz = os.path.getsize(fn)
        with open(fn, "rb") as f:
            if sz > max_bytes:
                f.seek(sz - max_bytes)
                f.readline()
            data = f.read()
        return data.decode("utf-8", "replace").splitlines()
    except OSError:
        return []


def nodes_info(root):
    """Per-node agent progress + agent totals (the old progress.html data)."""
    log = read_funnel_log(root)
    cut = 0
    for i in range(len(log) - 1, -1, -1):
        if log[i].startswith("RUN_START"):
            cut = i
            break
    window = log[cut:]
    nodes = {}
    for l in window:
        parts = l.split("\t")
        if l.startswith("NODE_START") and len(parts) >= 3:
            nodes.setdefault(parts[1], {"title": parts[2], "status": "running",
                                        "score": None, "kept": None, "stages": {},
                                        "aspects": 0})
        elif l.startswith("AGENT") and len(parts) >= 4:
            n = nodes.setdefault(parts[1], {"title": "", "status": "?", "score": None,
                                            "kept": None, "stages": {}, "aspects": 0})
            stage, ev = parts[2], parts[3]
            if stage == "PLANNER" and ev.startswith("DONE"):
                m = re.search(r"aspects=(\d+)", ev)
                n["aspects"] = int(m.group(1)) if m else 0
            if stage == "WORKER":
                aspect = ev.split(" ", 1)[1] if " " in ev else ""
                n["stages"]["W:" + aspect] = "active" if ev.startswith("START") else "done"
            else:
                n["stages"][stage] = "active" if ev.startswith("START") else \
                                     ("fail" if ev == "FAIL" else "done")
        elif l.startswith("NODE_DONE") and len(parts) >= 6:
            n = nodes.setdefault(parts[1], {"title": "", "status": "?", "score": None,
                                            "kept": None, "stages": {}, "aspects": 0})
            n["kept"] = parts[2]
            n["score"] = parts[4]
            n["status"] = "final" if parts[5] == "FINAL" else "retry"
        elif l.startswith("NODE_FAIL"):
            nodes.setdefault(parts[1], {"title": "", "status": "failed", "score": None,
                                        "kept": None, "stages": {}, "aspects": 0})["status"] = "failed"
    done_c, start_c = {}, {}
    for l in window:
        if not l.startswith("AGENT"):
            continue
        parts = l.split("\t")
        if len(parts) < 4:
            continue
        stage = "worker" if parts[2] == "WORKER" else parts[2].lower()
        ev = parts[3]
        if ev == "FAIL":
            continue
        if ev.startswith("DONE"):
            done_c[stage] = done_c.get(stage, 0) + 1
        elif ev.startswith("START"):
            start_c[stage] = start_c.get(stage, 0) + 1
    agents = {k: {"done": done_c.get(k, 0),
                  "running": max(0, start_c.get(k, 0) - done_c.get(k, 0))}
              for k in set(list(done_c) + list(start_c))}
    kept_live = atomic_live = calls_live = errors_live = 0
    for l in window:
        pp = l.split("\t")
        if l.startswith("NODE_DONE") and len(pp) >= 4:
            try:
                kept_live += int(pp[2]); atomic_live += int(pp[3])
            except ValueError:
                pass
        elif l.startswith("AGENT") and len(pp) >= 4:
            if pp[3].startswith("START"):
                calls_live += 1
            if pp[3] == "FAIL":
                errors_live += 1
    queued = 0
    for l in window:
        if l.startswith("QUEUE\t"):
            q = l.split("\t")
            if len(q) >= 2:
                try:
                    queued = int(q[1])
                except ValueError:
                    pass
    final_n = sum(1 for l in window if l.startswith("NODE_DONE") and "FINAL" in l)
    retry_n = sum(1 for l in window if l.startswith("NODE_DONE") and "RETRY-QUEUED" in l)
    order = []
    for l in window:
        if l.startswith("NODE_START"):
            p = l.split("\t")[1]
            if p not in order:
                order.append(p)
    recent = []
    for p in order[-12:]:
        n = nodes.get(p, {})
        stages = n.get("stages", {})
        w_done = sum(1 for k, v in stages.items() if k.startswith("W:") and v == "done")
        w_total = sum(1 for k in stages if k.startswith("W:"))
        w_active = any(v == "active" for k, v in stages.items() if k.startswith("W:"))

        def st(name):
            v = stages.get(name, "pending")
            return v if v in ("active", "done", "fail") else "pending"

        recent.append({"path": p, "title": n.get("title", ""), "status": n.get("status", "running"),
                       "score": n.get("score"), "kept": n.get("kept"),
                       "P": st("PLANNER"), "T": st("TRIMMER"), "C": st("CRITIQUE"), "S": st("SYNTHESIZER"),
                       "W_done": w_done, "W_total": w_total or n.get("aspects", 0),
                       "W_active": w_active})
    return {"queued": queued, "final": final_n, "retry": retry_n,
            "kept": kept_live, "atomic": atomic_live,
            "calls": calls_live, "errors": errors_live,
            "agents": agents, "nodes": recent, "started": len(order)}


def funnel_stats(root):
    log = read_funnel_log(root)
    cut = 0
    for i in range(len(log) - 1, -1, -1):
        if log[i].startswith("RUN_START"):
            cut = i
            break
    window = log[cut:]
    done = sum(1 for l in window if l.startswith("NODE_DONE"))
    fail = sum(1 for l in window if l.startswith("NODE_FAIL"))
    summary = None
    for l in reversed(log):
        if l.startswith("funneled="):
            summary = l
            break
    last = log[-1] if log else ""
    live = None
    for l in reversed(log):
        if l.startswith("THREADS\t"):
            parts = l.split("\t")
            if len(parts) >= 3:
                try: live = int(parts[2])
                except ValueError: pass
            break
    active_layer = None
    node_title = None
    node_path = None
    for l in reversed(log):
        if l.startswith("NODE_START"):
            parts = l.split("\t")
            if len(parts) >= 3:
                node_path = parts[1]
                node_title = parts[2]
                segs = node_path.count("-") + 1
                active_layer = f"L{max(0, segs - 1)}"
            break
    stage = None
    for l in reversed(log):
        if l.startswith("AGENT"):
            parts = l.split("\t")
            if len(parts) >= 4:
                stage = {"agent": parts[2], "ev": parts[3]}
            break
    return {"done": done, "fail": fail, "summary": summary, "last_line": last[-120:],
            "live_threads": live, "active_layer": active_layer,
            "node_title": node_title, "node_path": node_path, "stage": stage}


def _default_model():
    key = (os.environ.get("OPENROUTER_API_KEY")
           or os.environ.get("DEEPSEEK_API_KEY") or "")
    return "google/gemini-2.5-flash-lite" if key.startswith("sk-or") else "deepseek-flash"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--seed", default=None, help="funnel --root-node value")
    ap.add_argument("--branch", default=None)
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--deep", action="store_true",
                    help="depth-first funnel: descend each new chain (reaches L9 fast), "
                         "settle every node once (no retries)")
    ap.add_argument("--force", action="store_true",
                    help="re-run already-settled nodes (expand the subtree; "
                         "existing children are filtered, only new ones are kept)")
    ap.add_argument("--model", default=None,
                    help="model id (default: follows the installed key — "
                         "google/gemini-2.5-flash-lite for OpenRouter sk-or- keys, "
                         "deepseek-flash otherwise)")
    ap.add_argument("--initial", type=int, default=8)
    ap.add_argument("--threads", type=int, default=0,
                    help="pin the worker-thread count (1-5000); 0 = adaptive policy")
    ap.add_argument("--interval", type=int, default=2, help="sampling seconds")
    ap.add_argument("--hysteresis", type=int, default=30, help="min seconds between thread changes")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="127.0.0.1",
                    help="listen address (use 0.0.0.0 to expose on the network)")
    ap.add_argument("--no-server", action="store_true")
    ap.add_argument("--dry", action="store_true", help="sample+policy only, manage nothing")
    a = ap.parse_args()
    if not a.model:
        a.model = _default_model()
    root = a.root
    os.makedirs(os.path.join(root, "control"), exist_ok=True)   # fresh deployments

    sampler = Sampler()
    opencl_cache = {"t": 0.0, "info": {"available": False, "devices": []}}

    def opencl_info():
        now = time.time()
        if now - opencl_cache["t"] > 600:
            opencl_cache["t"] = now
            try:
                out = subprocess.run([os.path.join(root, "build/data_depot"),
                                      "gpu", "--json"],
                                     capture_output=True, text=True, timeout=5,
                                     cwd=root)
                opencl_cache["info"] = json.loads(out.stdout or "{}") or {
                    "available": False, "devices": []}
            except Exception:
                opencl_cache["info"] = {"available": False, "devices": []}
        return opencl_cache["info"]
    proc = None
    target = max(1, min(MAX_THREADS, a.initial))
    if a.threads:
        target = max(1, min(MAX_THREADS, a.threads))
    paused = False
    finished = False
    crash_count = 0
    last_rebuild = 0.0
    rebuild_proc = None
    last_change = 0.0
    history, policy = [], []

    def write_control():
        with open(os.path.join(root, "control/concurrency.txt"), "w") as f:
            f.write(f"{target}\n")

    def _override(root=ROOT):
        try:
            return int(open(os.path.join(root, "control/override.txt")).read().strip())
        except (OSError, ValueError):
            return None


    def write_state(s):
        st = {
                "ts": time.time(),
                "target_threads": target,
                "process": {"pid": proc.pid if proc else None,
                            "alive": proc is not None and proc.poll() is None,
                            "paused": paused},
                "gauges": {k: s[k] for k in
                           ("cores", "load1", "load5", "cpu_busy", "ram_avail_gb",
                            "net_rx_mbps", "net_tx_mbps", "link_mbps", "gpu_pct")},
                "history": history[-120:],
                "policy": policy[-50:],
                "funnel": funnel_stats(root),
                "opencl": opencl_info(),
                "progress": nodes_info(root),
                "layers": layer_counts(root),
                "run": {"seed": a.seed or ("branch: " + (a.branch or "P01-IN01-")),
                        "limit": a.limit, "rounds": a.rounds, "model": a.model},
                "flags": {"paused": os.path.exists(os.path.join(root, "control/paused")),
                          "stopped": os.path.exists(os.path.join(root, "control/stopped"))},
                "override": _override(root),
            }
        tmp = os.path.join(root, "state.json.tmp")
        with open(tmp, "w") as f:
            json.dump(st, f, indent=1)
        os.replace(tmp, os.path.join(root, "state.json"))

    def start_funnel():
        nonlocal proc
        cmd = [os.path.join(root, "build/data_depot"), "funnel",
               "--limit", str(a.limit), "--rounds", str(a.rounds),
               "--concurrency", str(target), "--model", a.model]
        if a.seed:
            cmd += ["--root-node", a.seed]
        else:
            cmd += ["--branch", a.branch or "P01-IN01-"]
        logpath = os.path.join(root, "funnel_run.log")
        try:
            if os.path.exists(logpath) and os.path.getsize(logpath) > 0:
                os.replace(logpath, os.path.join(root, "funnel_run.prev.log"))
        except OSError:
            pass
        if a.deep:
            cmd += ["--deep"]
        if a.force:
            cmd += ["--force"]
        logf = open(logpath, "a")
        proc = subprocess.Popen(cmd, cwd=root, stdout=logf, stderr=subprocess.STDOUT)
        logf.close()   # the child holds the fd; close our copy
        policy.append({"t": time.strftime("%H:%M:%S"), "event": f"started pid {proc.pid} cmd {' '.join(cmd[-6:])}"})

    def stop_funnel():
        nonlocal proc
        if proc and proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        proc = None
        policy.append({"t": time.strftime("%H:%M:%S"), "event": "stopped"})

    def on_sig(signum, frame):
        print("shutting down…", flush=True)
        stop_funnel()
        sys.exit(0)

    signal.signal(signal.SIGINT, on_sig)
    signal.signal(signal.SIGTERM, on_sig)

    def make_handler():
        class H(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def _json(self, obj, code=200):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def do_GET(self):
                path = urlparse(self.path).path
                if path == "/":          # bare address -> dashboard
                    path = "/dashboard.html"
                if path == "/api/state":
                    try:
                        return self._json(json.load(open(os.path.join(root, "state.json"))))
                    except (OSError, ValueError):
                        return self._json({"error": "no state yet"}, 404)
                if path == "/api/control":
                    paused = os.path.exists(os.path.join(root, "control/paused"))
                    override = None
                    try:
                        override = int(open(os.path.join(root, "control/override.txt")).read().strip())
                    except (OSError, ValueError):
                        pass
                    return self._json({"paused": paused, "override": override})
                name = os.path.basename(path) or "dashboard.html"
                if name.endswith(".js") and path.startswith("/three-addons/"):
                    pass  # vendored three.js example modules
                elif name not in ("dashboard.html", "depot_core_wasm.js", "three.module.js",
                                  "workflow_showcase.html", "progress.html", "favicon.ico",
                                  "Onion_Review.html"):
                    return self.send_error(403)   # only whitelisted assets are served
                fpath = os.path.normpath(os.path.join(root, path.lstrip("/")))
                if not fpath.startswith(root) or not os.path.isfile(fpath):
                    return self.send_error(404)
                ctype = ("text/html" if fpath.endswith(".html") else
                         "application/wasm" if fpath.endswith(".wasm") else
                         "application/javascript" if fpath.endswith(".js") else
                         "text/css" if fpath.endswith(".css") else "application/octet-stream")
                body = open(fpath, "rb").read()
                self.send_response(200)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def do_POST(self):
                path = urlparse(self.path).path
                try:
                    n = int(self.headers.get("Content-Length") or 0)
                    if n < 0 or n > 4096:
                        return self._json({"error": "body too large"}, 413)
                    data = json.loads(self.rfile.read(n) or b"{}")
                except (ValueError, json.JSONDecodeError):
                    data = {}
                if path == "/api/pause":
                    open(os.path.join(root, "control/paused"), "w").write("1")
                    try: os.remove(os.path.join(root, "control/stopped"))
                    except OSError: pass
                    return self._json({"paused": True})
                if path == "/api/stop":
                    open(os.path.join(root, "control/stopped"), "w").write("1")
                    try: os.remove(os.path.join(root, "control/paused"))
                    except OSError: pass
                    return self._json({"stopped": True})
                if path == "/api/resume":
                    for f in ("paused", "stopped"):
                        try: os.remove(os.path.join(root, f"control/{f}"))
                        except OSError: pass
                    open(os.path.join(root, "control/start.txt"), "w").write("1")
                    return self._json({"paused": False, "stopped": False})
                if path == "/api/threads":
                    try:
                        mx = int(data.get("max", 0) if isinstance(data, dict) else 0)
                    except (TypeError, ValueError):
                        return self._json({"error": "max must be an integer"}, 400)
                    if 1 <= mx <= MAX_THREADS:
                        open(os.path.join(root, "control/override.txt"), "w").write(str(mx))
                        return self._json({"override": mx})
                    try: os.remove(os.path.join(root, "control/override.txt"))
                    except OSError: pass
                    return self._json({"override": None})
                return self.send_error(404)
        return H

    if not a.no_server and not a.dry:
        srv = ThreadingHTTPServer((a.bind, a.port), make_handler())
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        print(f"dashboard: http://<this-machine>:{a.port}/dashboard.html", flush=True)

    print(f"controller: cores={sampler.cores} base={min(16, max(2, sampler.cores))} "
          f"threads={'PINNED ' + str(target) if a.threads else 'adaptive from ' + str(target)} "
          f"interval={a.interval}s hysteresis={a.hysteresis}s "
          f"link={'?' if sampler.link_mbps is None else str(sampler.link_mbps) + ' Mbps'}",
          flush=True)
    if not a.dry:
        write_control()
        # a cap set in a previous session must not silently bound a fresh run
        # (e.g. pinning --threads 16 but a stale override.txt=8 caps it)
        try:
            os.remove(os.path.join(root, "control/override.txt"))
            print("[controller] cleared stale thread-cap override", flush=True)
        except OSError:
            pass

    while True:
        s = sampler.sample()
        prev = target
        if not paused:
            target = compute_target(s, target)
        if a.threads:
            target = max(1, min(MAX_THREADS, a.threads))   # pinned: no adaptation
        # manual override (dashboard): cap the target
        try:
            ov = int(open(os.path.join(root, "control/override.txt")).read().strip())
            target = min(target, ov)
        except (OSError, ValueError):
            pass
        if target != prev and (time.time() - last_change >= a.hysteresis or target < prev):
            last_change = time.time()
            if not a.dry:
                write_control()   # only commit changes — the funnel reconciles this file
            why = []
            if s["load1"] > 0.8 * s["cores"]: why.append("busy")
            elif s["load1"] < 0.3 * s["cores"]: why.append("idle")
            if s["ram_avail_gb"] < 1.0: why.append("low-ram")
            if s.get("link_mbps") and s["net_rx_mbps"] + s["net_tx_mbps"] > 0.7 * s["link_mbps"]:
                why.append("nic-sat")
            policy.append({"t": time.strftime("%H:%M:%S"),
                           "event": f"threads {prev} -> {target} ({','.join(why) or 'policy'})"})
            print(f"[policy] threads {prev} -> {target} ({','.join(why) or 'policy'})")

        history.append({"t": time.strftime("%H:%M:%S"), "load": round(s["load1"], 2),
                        "cpu": round(s["cpu_busy"], 1), "ram": round(s["ram_avail_gb"], 1),
                        "net": round(s["net_rx_mbps"] + s["net_tx_mbps"], 1),
                        "threads": target})

        # manage the funnel process
        if not a.dry:
            alive = proc is not None and proc.poll() is None
            pflag = os.path.exists(os.path.join(root, "control/paused"))
            sflag = os.path.exists(os.path.join(root, "control/stopped"))
            if (pflag or sflag) and not paused:
                stop_funnel(); paused = True
                policy.append({"t": time.strftime("%H:%M:%S"),
                               "event": "paused" if pflag else "stopped (no auto-restart)"})
            elif not (pflag or sflag) and paused:
                paused = False
                policy.append({"t": time.strftime("%H:%M:%S"), "event": "resumed / started"})
            start_flag = os.path.join(root, "control/start.txt")
            if os.path.exists(start_flag):
                try: os.remove(start_flag)
                except OSError: pass
                finished = False
                paused = False
                policy.append({"t": time.strftime("%H:%M:%S"), "event": "manual start requested"})
            if not paused and not alive and proc is None and not finished:
                start_funnel()
            elif not paused and proc and proc.poll() is not None:
                rc = proc.returncode
                policy.append({"t": time.strftime("%H:%M:%S"),
                               "event": f"funnel exited rc={rc}"})
                if rc == 0:
                    finished = True   # clean finish -> do NOT restart
                    crash_count = 0
                    print("[controller] funnel finished cleanly (rc=0) — idle until re-seeded", flush=True)
                else:
                    crash_count += 1
                    if crash_count >= 5:
                        finished = True   # give up: a persistent crash must not
                        print("[controller] funnel crash-loop detected (5 restarts) — giving up; "
                              "fix and re-seed", flush=True)
                    else:
                        time.sleep(10)    # crash backoff, then restart
                proc = None
            if not a.dry and target != prev:
                write_control()

        write_state(s)
        # tree viewer rebuild every ~60s — ASYNC: never blocks state updates
        if (not a.dry and time.time() - last_rebuild >= 60
                and (rebuild_proc is None or rebuild_proc.poll() is not None)):
            last_rebuild = time.time()
            rebuild_proc = subprocess.Popen(
                ["sh", "-c", "python3 rebuild_json.py && python3 gen_onion_html.py"],
                cwd=root, env={**os.environ, "ONION_ROOT": root},
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(a.interval)


if __name__ == "__main__":
    main()
