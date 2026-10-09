# CLI Dashboard — Extended Research (tools & GitHub repos)

Goal: rebuild the Onion Mission Control dashboard as a **terminal (CLI/TUI)** app,
consuming the exact same `/api/state` JSON the controller already serves
(no changes to the C++ core or the controller API).

Data gathered 2026-10-03 from the GitHub API + repo pages (stars/activity/license).

## 1. TUI frameworks (widgets & layout)

| Repo | Lang | Stars | License | Last push | Notes |
|---|---|---|---|---|---|
| [ratatui/ratatui](https://github.com/ratatui/ratatui) | Rust | 22.9k | MIT | 2026-09 | The standard. Widgets, **Canvas** (2D drawing → our rings), Sparkline, Chart, async |
| [Textualize/textual](https://github.com/Textualize/textual) | Python | 37.4k | MIT | 2026-07 | CSS-like styling, reactive; fastest to build (controller is already Python); `textual-plotext` for charts |
| [charmbracelet/bubbletea](https://github.com/charmbracelet/bubbletea) | Go | 45.3k | MIT | 2026-10 | Elm architecture; + [lipgloss](https://github.com/charmbracelet/lipgloss) (12k★) + bubbles widgets |
| [rivo/tview](https://github.com/rivo/tview) | Go | 14.1k | MIT | 2026-08 | Classic widgets (k9s uses it) |
| [vadimdemedes/ink](https://github.com/vadimdemedes/ink) | TypeScript | 40.0k | MIT | 2026-10 | React for CLIs |
| [peterbrittain/asciimatics](https://github.com/peterbrittain/asciimatics) | Python | 4.3k | Apache-2.0 | 2026-07 | Older but complete |
| [jroimartin/gocui](https://github.com/jroimartin/gocui) | Go | 10.6k | BSD-3 | 2025-05 | Less active now |

## 2. Monitoring dashboards (direct analogues of our screen)

| Repo | Lang | Stars | Why it matters |
|---|---|---|---|
| [ClementTsang/bottom](https://github.com/ClementTsang/bottom) (`btm`) | Rust | 14.1k | CPU/RAM/NET gauges + graphs + per-process tables — same layout as ours; best reference |
| [aristocratos/btop](https://github.com/aristocratos/btop) | C++ | 34.8k | C++ monitoring TUI — closest to our C++ core; a reference for C++ layout code, but too big to extend |

## 3. Charts / sparklines in terminal

- **plotext** (Python, ~2.4k★) — line/bar/braille charts, pairs with Textual
- **ntcharts** (Go) — sparkline/bar/line widgets for bubbletea
- ratatui built-ins: `Sparkline`, `Chart`, `Gauge` — no extra deps

## 4. 3D in the terminal (for the onion rings)

| Repo | Lang | Stars | Notes |
|---|---|---|---|
| [wojciech-graj/TermGL](https://github.com/wojciech-graj/TermGL) | **C** | 409 | "2D & 3D graphics engine in the terminal [C/C++]" — native C++, can render **rotating rings**; callable from our C++ core |
| [dankamongmen/notcurses](https://github.com/dankamongmen/notcurses) | C | 4.7k | Multimedia TUI: 3D, plots, images, mouse — heavyweight but very capable |
| [Cubified/termgl](https://github.com/Cubified/termgl) | C | 12 | Shadertoy-style GLSL in the terminal |
| [HunterLiles/terminal-renderer](https://github.com/HunterLiles/terminal-renderer) | C | ~0 | Minimal C TUI 3D renderer (learning reference) |
| ratatui `Canvas` widget | Rust | — | 2D projected ellipses (our rings are 2D projections anyway) — pragmatic |

## 5. Recommendation for THIS project

| Option | Stack | Effort | Fit |
|---|---|---|---|
| **A — Textual (Python)** | textual + plotext + httpx | **Low** (1 file, reuse controller code) | Merge as `controller.py --tui`: same process can poll state internally, zero HTTP, auto-starts with the dashboard. Rings drawn as 2D braille-ellipses. |
| **B — Ratatui (Rust)** | ratatui + reqwest/tokio | Medium | New `data_depot_tui` crate/binary; Canvas rings, Sparkline gauges, best polish; single static binary for the VPS. |
| **C — C++ native** | TermGL (or notcurses) + our core | Medium-High | True 3D rings in terminal from the C++ side; TermGL is unmaintained-ish (409★, few commits) — risk. |

**Pick: A (Textual)** — fastest path, one language, ships with the controller.
If you want a standalone compiled binary and the most polished widgets → **B (Ratatui)**.
C only if real terminal-3D is the whole point.

## 6. Proposed CLI design (works for A or B)

```
┌ Machine Load ────────────┬─────────────┐
│ load 2.48 cpu 20% ram 6.1│  ( ( (○) ) )│   ← braille-projected rings,
│ threads 4/12  net 2      │   ( ((●)) )│     clickable via mouse mode,
│ ▂▃▅▃▂ (sparkline)       │  rings L0-L9 │     active band pulses
├ Funnel ──────────────────┼─────────────┤
│ root T30 · node X · step CRITIQUE      │
│ loop 7/10 ██████░░ 70% · queued 973    │
│ P✓ W8/16 ●T ○C ○S · score 0.72         │
├ Control ── [P]ause [S]top [Start] cap:8│
└ Keys: q quit · p pause · r run · +/- zoom │
```
All data comes from `/api/state` (3s poll) — the same JSON the web dashboard uses.

## 7. Sourcing notes
- GitHub API + repo pages fetched 2026-10-03; stars rounded.
- TermGL original repo (JustWhit3/TermGL) is 404 — the active fork is `wojciech-graj/TermGL`.
