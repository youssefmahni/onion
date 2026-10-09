// onion_gl.cpp — Onion Mission Control, native OpenGL edition (C++17 + Xlib/GLX).
//
// Reads the same /api/state JSON as the web dashboard and the Textual TUI.
// Runs on any machine with a display; point it at the VPS with --url.
//
//   ./build/onion_gl                                  # local controller (:8080 or $ONION_PORT)
//   ./build/onion_gl --url http://10.8.0.72:8090/api/state
//   ./build/onion_gl --selftest                       # headless geometry/JSON check
//
// Keys: q quit · p pause · s start/continue · t stop · 0-9 select ring ·
//       c clear · +/- zoom · [ ] thread cap · mouse: click a ring, wheel zooms

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <csignal>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <vector>
#include <unistd.h>

#ifdef ONION_HAVE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "json.hpp"

using json = nlohmann::json;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------- constants
static const float LAYER_RGB[10][3] = {
    {139/255.f,  92/255.f, 246/255.f},  // L0 #8b5cf6
    { 16/255.f, 185/255.f, 129/255.f},  // L1 #10b981
    { 59/255.f, 130/255.f, 246/255.f},  // L2 #3b82f6
    {  6/255.f, 182/255.f, 212/255.f},  // L3 #06b6d4
    {132/255.f, 204/255.f,  22/255.f},  // L4 #84cc16
    {236/255.f,  72/255.f, 153/255.f},  // L5 #ec4899
    {217/255.f,  70/255.f, 239/255.f},  // L6 #d946ef
    {244/255.f,  63/255.f,  94/255.f},  // L7 #f43f5e
    {251/255.f, 146/255.f,  60/255.f},  // L8 #fb923c
    {234/255.f, 179/255.f,   8/255.f},  // L9 #eab308
};
static const char* LAYER_NAMES[10] = {
    "Phases", "Artifacts", "Tasks", "Sub-tasks", "Task Details",
    "Items", "Sub-items", "L7", "L8", "L9"};
static const char* LAYER_DESC[10] = {
    "Program phases - the top-level roadmap.",
    "Artifacts - inputs consumed and outputs produced.",
    "Tasks - blocks of work inside an artifact.",
    "Sub-tasks - sequenced steps within a task.",
    "Task details - verb-level activities.",
    "Items - the knowledge payload.",
    "Sub-items - nested decompositions.",
    "Layer 7 - deeper nesting (reserved).",
    "Layer 8 - deeper nesting (reserved).",
    "Layer 9 - deepest nesting (hard cap)."};

static const int CAP_PRESETS[] = {0, 1, 4, 8, 16, 32, 64, 100, 150, 250, 500, 1000, 2000, 5000};
static const int CAP_N = (int)(sizeof(CAP_PRESETS) / sizeof(CAP_PRESETS[0]));

// ring geometry in "dot units" (same layout as the terminal TUI)
static inline double ring_mid(int i)  { return 8.0 + i * 4.2 + 1.3; }
static inline double ring_band(int i) { return 2.6; }
static const double RING_MAX = 8.0 + 9 * 4.2 + 2.6;

// ---------------------------------------------------------------- snapshot
struct NodeRow {
    std::string title, status;
    float score = -1.f;
    int kept = -1, w_done = 0, w_total = 0;
    int st[4] = {0, 0, 0, 0};  // P T C S: 0 pending, 1 active, 2 done, 3 fail
};

struct Snapshot {
    bool ok = false;
    double ts = 0;
    long long counts[10] = {0};
    long long target_threads = 8, live_threads = -1;
    long long done = 0, limit = 0, queued = 0, fails = 0;
    long long kept_total = -1, atomic_total = -1, calls = -1, retried = -1, errors = -1;
    int override_ = -1;                       // -1 = none
    bool alive = false, paused = false, stopped = false;
    long pid = 0;
    std::string seed, node_title, stage_agent, stage_ev, last_line, active_layer;
    double load1 = 0, load5 = 0, cpu = 0, ram = 0, net = 0, gpu = -1;
    int cores = 0;
    std::vector<std::array<float, 5>> hist;   // load, cpu, ram, net, threads
    long long agents[5] = {0, 0, 0, 0, 0};    // done P T C S
    std::vector<NodeRow> nodes;
};

static long long statv(const json& j, const char* k, long long def) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<long long>();
}
static double statd(const json& j, const char* k, double def) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<double>();
}
static std::string stats(const json& j, const char* k, const char* def) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

static Snapshot parse_state(const json& j) {
    Snapshot s;
    s.ok = true;
    s.ts = statd(j, "ts", 0);
    s.target_threads = statv(j, "target_threads", 8);
    s.override_ = (int)statv(j, "override", -1);

    if (j.contains("layers") && j["layers"].is_object())
        for (int i = 0; i < 10; i++)
            s.counts[i] = statv(j["layers"], ("L" + std::to_string(i)).c_str(), 0);

    if (j.contains("process") && j["process"].is_object()) {
        const auto& p = j["process"];
        s.pid = statv(p, "pid", 0);
        auto it = p.find("alive");
        s.alive = it != p.end() && it->is_boolean() && it->get<bool>();
    }
    if (j.contains("flags") && j["flags"].is_object()) {
        const auto& f = j["flags"];
        auto it = f.find("paused");
        s.paused = it != f.end() && it->is_boolean() && it->get<bool>();
        it = f.find("stopped");
        s.stopped = it != f.end() && it->is_boolean() && it->get<bool>();
    }
    if (j.contains("gauges") && j["gauges"].is_object()) {
        const auto& g = j["gauges"];
        s.load1 = statd(g, "load1", 0);   s.load5 = statd(g, "load5", 0);
        s.cpu  = statd(g, "cpu_busy", 0); s.ram  = statd(g, "ram_avail_gb", 0);
        s.net  = statd(g, "net_rx_mbps", 0) + statd(g, "net_tx_mbps", 0);
        s.gpu  = statd(g, "gpu_pct", -1);
        s.cores = (int)statv(g, "cores", 0);
    }
    if (j.contains("history") && j["history"].is_array()) {
        const auto& h = j["history"];
        size_t start = h.size() > 120 ? h.size() - 120 : 0;
        for (size_t i = start; i < h.size(); i++) {
            const auto& e = h[i];
            if (!e.is_object()) continue;
            s.hist.push_back({(float)statd(e, "load", 0), (float)statd(e, "cpu", 0),
                              (float)statd(e, "ram", 0),  (float)statd(e, "net", 0),
                              (float)statd(e, "threads", 0)});
        }
    }
    if (j.contains("run") && j["run"].is_object()) {
        s.seed = stats(j["run"], "seed", "");
        s.limit = statv(j["run"], "limit", 0);
    }
    if (j.contains("funnel") && j["funnel"].is_object()) {
        const auto& f = j["funnel"];
        s.done = statv(f, "done", 0);
        s.fails = statv(f, "fail", 0);
        s.live_threads = statv(f, "live_threads", -1);
        s.node_title = stats(f, "node_title", "");
        s.last_line = stats(f, "last_line", "");
        s.active_layer = stats(f, "active_layer", "");
        if (f.contains("stage") && f["stage"].is_object()) {
            s.stage_agent = stats(f["stage"], "agent", "");
            s.stage_ev = stats(f["stage"], "ev", "");
        }
        if (f.contains("summary") && f["summary"].is_string()) {
            const std::string sm = f["summary"].get<std::string>();
            auto get = [&](const char* k, long long& out) {
                const std::string pat = std::string(k) + "=";
                size_t p = sm.find(pat);
                if (p == std::string::npos) return;
                out = std::atoll(sm.c_str() + p + pat.size());
            };
            get("funneled", s.done); get("kept", s.kept_total);
            get("atomic", s.atomic_total); get("calls", s.calls);
            get("retried", s.retried); get("errors", s.errors);
        }
    }
    if (j.contains("progress") && j["progress"].is_object()) {
        const auto& pr = j["progress"];
        s.queued = statv(pr, "queued", 0);
        if (pr.contains("agents") && pr["agents"].is_object()) {
            static const char* keys[5] = {"planner", "worker", "trimmer", "critique", "synthesizer"};
            for (int k = 0; k < 5; k++)
                if (pr["agents"].contains(keys[k]) && pr["agents"][keys[k]].is_object())
                    s.agents[k] = statv(pr["agents"][keys[k]], "done", 0);
        }
        if (pr.contains("nodes") && pr["nodes"].is_array()) {
            const auto& nl = pr["nodes"];
            size_t start = nl.size() > 6 ? nl.size() - 6 : 0;
            for (size_t i = start; i < nl.size(); i++) {
                const auto& n = nl[i];
                if (!n.is_object()) continue;
                NodeRow r;
                r.title = stats(n, "title", "");
                if (r.title.empty()) r.title = stats(n, "path", "?");
                r.status = stats(n, "status", "running");
                double sc = statd(n, "score", -1);
                r.score = (float)sc;
                r.kept = (int)statv(n, "kept", -1);
                r.w_done = (int)statv(n, "W_done", 0);
                r.w_total = (int)statv(n, "W_total", 0);
                static const char* sk[4] = {"P", "T", "C", "S"};
                for (int k = 0; k < 4; k++) {
                    std::string v = stats(n, sk[k], "pending");
                    r.st[k] = v == "done" ? 2 : v == "active" ? 1 : v == "fail" ? 3 : 0;
                }
                s.nodes.push_back(std::move(r));
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------- fonts
struct FontAtlas {
    GLuint tex = 0;
    stbtt_bakedchar cdata[96];
    int w = 0, h = 0;
    float size = 0;
    bool ready() const { return tex != 0; }
};

static bool bake_font(FontAtlas& f, const char* path, float px) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 100) return false;
    f.w = f.h = 1024;
    std::vector<unsigned char> bmp((size_t)f.w * f.h, 0);
    int rc = stbtt_BakeFontBitmap((const unsigned char*)data.data(), 0, px,
                                  bmp.data(), f.w, f.h, 32, 96, f.cdata);
    if (rc <= 0) return false;   // atlas too small (won't happen at our sizes)
    // LUMINANCE_ALPHA so glColor tints the glyphs correctly under GL_MODULATE
    std::vector<unsigned char> la((size_t)f.w * f.h * 2);
    for (size_t i = 0; i < (size_t)f.w * f.h; i++) {
        la[i * 2] = bmp[i];
        la[i * 2 + 1] = bmp[i];
    }
    glGenTextures(1, &f.tex);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, f.w, f.h, 0,
                 GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, la.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    f.size = px;
    return true;
}

static int gW = 1280, gH = 800;

static inline void vtx(float px, float py) {
    glVertex2f(px - gW / 2.0f, gH / 2.0f - py);
}

static void draw_text(const FontAtlas& f, float px, float py, float r, float g, float b, float a,
                      const std::string& text) {
    if (!f.ready() || text.empty()) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    float x = px, y = py;
    for (unsigned char ch : text) {
        if (ch < 32 || ch > 126) ch = '?';
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(f.cdata, f.w, f.h, ch - 32, &x, &y, &q, 1);
        glTexCoord2f(q.s0, q.t0); vtx(q.x0, q.y0);
        glTexCoord2f(q.s1, q.t0); vtx(q.x1, q.y0);
        glTexCoord2f(q.s1, q.t1); vtx(q.x1, q.y1);
        glTexCoord2f(q.s0, q.t1); vtx(q.x0, q.y1);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

static inline float text_w(const FontAtlas& f, const std::string& s) {
    // approximate: baked chars carry advance via GetBakedQuad only; use size*0.62
    return (float)s.size() * f.size * 0.62f;
}

// ---------------------------------------------------------------- http
static std::string http_get(const std::string& url) {
    std::string cmd = "curl -4 -s --max-time 3 '" + url + "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

static void http_post(const std::string& url, const std::string& body) {
    std::string cmd = "curl -4 -s -o /dev/null --max-time 3 -X POST";
    if (!body.empty())
        cmd += " -H 'Content-Type: application/json' -d '" + body + "'";
    cmd += " '" + url + "'";
    std::system(cmd.c_str());
}

// ---------------------------------------------------------------- geometry
static void build_dot_positions(int layer, long long n, std::vector<float>& out) {
    out.clear();
    if (n <= 0) return;
    out.resize((size_t)n * 2);
    const double r = ring_mid(layer);
    const double step = 2.0 * M_PI / (double)n;
    const double ca = std::cos(step), sa = std::sin(step);
    double c = 1.0, s = 0.0;
    for (long long j = 0; j < n; j++) {
        out[(size_t)j * 2]     = (float)(c * r);
        out[(size_t)j * 2 + 1] = (float)(s * r);
        double nc = c * ca - s * sa;
        s = s * ca + c * sa;
        c = nc;
    }
}

// ---------------------------------------------------------------- app state
struct App {
    Display* dpy = nullptr;
    Window win = 0;
    GLXContext ctx = nullptr;
    Atom wm_delete = 0;
    std::string url;
    bool no_spawn = false;
    std::string seed;
    int limit = 0, port = 8080;
    bool local = false;
    bool spawned = false;
    std::chrono::steady_clock::time_point t0;

    std::mutex mtx;
    Snapshot snap;
    std::atomic<bool> running{true};

    int selected = -1;
    float zoom = 1.0f;
    int cap_idx = 0;
    bool dirty = true;

    FontAtlas f_main, f_head;

    std::vector<std::vector<float>> dots;   // per-layer dot positions
    long long built[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};

static App app;

// terminal-mode switches (set by main before run_term)
static int g_term_mode = -1;     // -1 off, 0 sixel, 1 blocks
static bool g_term_once = false;
static bool g_term_force = false;
static float g_term_scale = 1.0f;
static int g_term_cols = 0, g_term_rows = 0;
static const char* g_font_path = nullptr;
static const char* DEF_FONTS[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};

// ---------------------------------------------------------------- rendering
struct RingLayout {
    float cx, cy, s, asp, panelW;
};

static RingLayout layout() {
    RingLayout L{};
    L.panelW = std::max(430.0f, std::min(560.0f, gW * 0.36f));
    float rw = gW - L.panelW - 30.0f;
    float rh = (float)gH - 96.0f;
    L.cx = L.panelW + 15.0f + rw * 0.5f;
    L.cy = 56.0f + rh * 0.5f;
    L.asp = std::max(1.0f, std::min(2.4f, (rw / std::max(1.0f, rh)) * 0.62f));
    float fit_h = (rh * 0.5f - 24.0f) / (float)RING_MAX;
    float fit_w = (rw * 0.5f - 24.0f) / ((float)RING_MAX * L.asp);
    L.s = std::max(0.3f, std::min(fit_h, fit_w));
    return L;
}

static void draw_ring_outline(const RingLayout& L, int i, float alpha) {
    glColor4f(LAYER_RGB[i][0], LAYER_RGB[i][1], LAYER_RGB[i][2], alpha);
    glBegin(GL_LINE_LOOP);
    for (int k = 0; k < 200; k++) {
        double a = 2.0 * M_PI * k / 200.0;
        float x = L.cx + (float)(std::cos(a) * ring_mid(i) * L.s * L.asp * app.zoom);
        float y = L.cy - (float)(std::sin(a) * ring_mid(i) * L.s * app.zoom);
        vtx(x, y);
    }
    glEnd();
}

static void draw_dots(const RingLayout& L, int i) {
    if (app.dots[i].empty()) return;
    int active = -1;
    if (app.snap.ok && app.snap.alive && app.snap.active_layer.size() == 2)
        active = app.snap.active_layer[1] - '0';
    float boost = (i == active) ? 1.35f : 1.0f;
    bool sel = (i == app.selected);
    if (sel) glColor4f(1.f, 1.f, 1.f, 0.95f);
    else     glColor4f(LAYER_RGB[i][0], LAYER_RGB[i][1], LAYER_RGB[i][2], 0.95f);
    glPointSize((sel ? 3.2f : 2.3f) * boost);
    glPushMatrix();
    glTranslatef(L.cx - gW / 2.0f, gH / 2.0f - L.cy, 0.f);
    glScalef(L.s * L.asp * app.zoom, L.s * app.zoom, 1.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, app.dots[i].data());
    glDrawArrays(GL_POINTS, 0, (GLsizei)(app.dots[i].size() / 2));
    glDisableClientState(GL_VERTEX_ARRAY);
    glPopMatrix();
}

static void draw_series(const std::vector<float>& v, float x, float y, float w, float h,
                        float r, float g, float b) {
    if (v.size() < 2) return;
    float lo = *std::min_element(v.begin(), v.end());
    float hi = *std::max_element(v.begin(), v.end());
    float rng = (hi - lo) > 1e-6f ? (hi - lo) : 1.0f;
    glColor4f(r, g, b, 0.9f);
    glBegin(GL_LINE_STRIP);
    for (size_t i = 0; i < v.size(); i++) {
        float px = x + (float)i / (float)(v.size() - 1) * w;
        float py = y + h - (v[i] - lo) / rng * h;
        vtx(px, py);
    }
    glEnd();
}

static void hud() {
    const Snapshot& s = app.snap;
    const FontAtlas& F = app.f_main;
    const FontAtlas& H = app.f_head;
    auto put = [&](float x, float y, float r, float g, float b, const std::string& t) {
        draw_text(F, x, y, r, g, b, 1.f, t);
    };
    auto head = [&](float x, float y, float r, float g, float b, const std::string& t) {
        draw_text(H, x, y, r, g, b, 1.f, t);
    };

    char buf[512];
    // ---- top bar
    float x = 16, y = 10;
    auto seg = [&](const char* text, float r, float g, float b, float pad = 18.f) {
        put(x, y, r, g, b, text);
        x += text_w(F, text) + pad;
    };
    std::snprintf(buf, sizeof buf, "LOAD %.2f", s.load1);      seg(buf, 0.13f, 0.83f, 0.93f);
    std::snprintf(buf, sizeof buf, "CPU %.0f%%", s.cpu);       seg(buf, 0.65f, 0.55f, 0.98f);
    std::snprintf(buf, sizeof buf, "RAM %.1fGB", s.ram);       seg(buf, 0.53f, 0.94f, 0.67f);
    std::snprintf(buf, sizeof buf, "NET %.0f", s.net);         seg(buf, 0.98f, 0.75f, 0.14f);
    if (s.gpu >= 0) { std::snprintf(buf, sizeof buf, "GPU %.0f%%", s.gpu); seg(buf, 0.98f, 0.75f, 0.14f); }
    std::snprintf(buf, sizeof buf, "THREADS %lld/%lld", s.target_threads,
                  s.live_threads >= 0 ? s.live_threads : 0);
    seg(buf, 0.56f, 0.71f, 0.98f);
    std::snprintf(buf, sizeof buf, "pid %ld", s.pid);
    seg(buf, 0.56f, 0.63f, 0.75f);

    const char* status = !s.ok ? "NO CONTROLLER" : s.alive ? "RUNNING"
                        : s.paused ? "PAUSED" : s.stopped ? "STOPPED" : "IDLE - finished";
    float sr = !s.ok ? 0.95f : s.alive ? 0.53f : s.paused ? 0.98f : 0.98f;
    float sg = !s.ok ? 0.35f : s.alive ? 0.94f : s.paused ? 0.75f : 0.35f;
    float sb = !s.ok ? 0.35f : s.alive ? 0.67f : s.paused ? 0.15f : 0.35f;
    put(x + 8, y, sr, sg, sb, std::string(status));

    // ---- left column
    float lx = 16, ly = 42, lw = layout().panelW - 32;
    head(lx, ly, 0.13f, 0.83f, 0.93f, "Machine Load");
    ly += 24;
    std::snprintf(buf, sizeof buf, "load1 %.2f   load5 %.2f   cores %d", s.load1, s.load5, s.cores);
    put(lx, ly, 0.56f, 0.63f, 0.75f, buf);
    ly += 20;

    struct Ser { const char* lbl; int idx; float r, g, b; };
    Ser series[5] = {{"load", 0, 0.13f, 0.83f, 0.93f}, {"cpu", 1, 0.65f, 0.55f, 0.98f},
                     {"ram", 2, 0.53f, 0.94f, 0.67f},  {"net", 3, 0.98f, 0.75f, 0.14f},
                     {"thr", 4, 0.96f, 0.45f, 0.71f}};
    for (const auto& S : series) {
        put(lx, ly - 1, 0.56f, 0.63f, 0.75f, S.lbl);
        std::vector<float> v;
        v.reserve(s.hist.size());
        for (const auto& h : s.hist) v.push_back(h[S.idx]);
        if (!v.empty()) {
            std::snprintf(buf, sizeof buf, "%6.1f", v.back());
            put(lx + 34, ly - 1, S.r, S.g, S.b, buf);
        }
        draw_series(v, lx + 96, ly, lw - 96, 18, S.r, S.g, S.b);
        ly += 22;
    }

    ly += 8;
    head(lx, ly, 0.13f, 0.83f, 0.93f, "Funnel - live run");
    ly += 24;
    std::snprintf(buf, sizeof buf, "root  %s", s.seed.empty() ? "-" : s.seed.c_str());
    put(lx, ly, 0.56f, 0.63f, 0.75f, buf); ly += 18;
    std::snprintf(buf, sizeof buf, "node  %s", s.node_title.empty() ? "-" : s.node_title.c_str());
    put(lx, ly, 0.91f, 0.93f, 0.97f, buf); ly += 18;
    if (!s.stage_agent.empty()) {
        std::snprintf(buf, sizeof buf, "step  %s %s", s.stage_agent.c_str(), s.stage_ev.c_str());
        put(lx, ly, 0.13f, 0.83f, 0.93f, buf);
    }
    ly += 18;
    std::snprintf(buf, sizeof buf, "loop  %lld/%lld    queued %lld    fails %lld",
                  s.done, s.limit ? s.limit : 0, s.queued, s.fails);
    put(lx, ly, 0.91f, 0.93f, 0.97f, buf); ly += 18;
    std::snprintf(buf, sizeof buf, "agents P%lld W%lld T%lld C%lld S%lld",
                  s.agents[0], s.agents[1], s.agents[2], s.agents[3], s.agents[4]);
    put(lx, ly, 0.13f, 0.83f, 0.93f, buf); ly += 18;
    if (s.kept_total >= 0) {
        std::snprintf(buf, sizeof buf, "kept %lld  atomic %lld  calls %lld  ret %lld  err %lld",
                      s.kept_total, s.atomic_total, s.calls, s.retried, s.errors);
        put(lx, ly, 0.96f, 0.45f, 0.71f, buf); ly += 18;
    }
    if (!s.last_line.empty()) {
        std::string l = s.last_line;
        if (l.size() > 74) l = l.substr(0, 71) + "...";
        put(lx, ly, 0.37f, 0.44f, 0.59f, l); ly += 18;
    }

    ly += 8;
    head(lx, ly, 0.13f, 0.83f, 0.93f, "Nodes - agent progress");
    ly += 24;
    for (const auto& n : s.nodes) {
        float r = 0.58f, g = 0.77f, b = 0.99f;
        if (n.status == "final")      { r = 0.53f; g = 0.94f; b = 0.67f; }
        else if (n.status == "retry") { r = 0.98f; g = 0.75f; b = 0.14f; }
        else if (n.status == "failed"){ r = 0.99f; g = 0.64f; b = 0.69f; }
        std::string t = n.title;
        if (t.size() > 46) t = t.substr(0, 43) + "...";
        put(lx, ly, r, g, b, t); ly += 16;
        auto mark = [](int v) { return v == 2 ? '+' : v == 1 ? '*' : v == 3 ? 'x' : '.'; };
        std::snprintf(buf, sizeof buf, "   %c%c%c%c  W%d/%d  score %.2f  kept %d",
                      mark(n.st[0]), mark(n.st[1]), mark(n.st[2]), mark(n.st[3]),
                      n.w_done, n.w_total, n.score, n.kept);
        put(lx + 8, ly, 0.56f, 0.63f, 0.75f, buf); ly += 16;
        if (ly > gH - 120) break;
    }
    if (s.nodes.empty()) { put(lx, ly, 0.56f, 0.63f, 0.75f, "no nodes in this run"); ly += 16; }

    // ---- layer card (bottom-left)
    float cy = (float)gH - 96;
    if (app.selected >= 0) {
        int i = app.selected;
        std::snprintf(buf, sizeof buf, "L%d - %s", i, LAYER_NAMES[i]);
        head(lx, cy, LAYER_RGB[i][0], LAYER_RGB[i][1], LAYER_RGB[i][2], buf); cy += 22;
        std::snprintf(buf, sizeof buf, "nodes %lld   depth %d/9", s.counts[i], i);
        put(lx, cy, 0.56f, 0.63f, 0.75f, buf); cy += 18;
        put(lx, cy, 0.56f, 0.63f, 0.75f, LAYER_DESC[i]); cy += 18;
        if (s.alive && s.active_layer == "L" + std::to_string(i))
            put(lx, cy, 0.13f, 0.83f, 0.93f, "BEING WRITTEN NOW");
    } else {
        put(lx, cy, 0.13f, 0.83f, 0.93f, "onion legend - click a ring or press 0-9");
    }

    // ---- cap + keys (bottom bar)
    std::snprintf(buf, sizeof buf, "cap: %s", s.override_ > 0 ? std::to_string(s.override_).c_str() : "auto");
    put(lx, (float)gH - 26, 0.65f, 0.55f, 0.98f, buf);
    const char* keys = "q quit  p pause  s start/continue  t stop  0-9 ring  c clear  +/- zoom  [ ] cap";
    put(gW - text_w(F, keys) - 16, (float)gH - 26, 0.37f, 0.44f, 0.59f, keys);
}

static void render() {
    glViewport(0, 0, gW, gH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-gW / 2.0, gW / 2.0, -gH / 2.0, gH / 2.0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.02f, 0.03f, 0.06f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);

    {
        std::lock_guard<std::mutex> lk(app.mtx);
        RingLayout L = layout();
        for (int i = 0; i < 10; i++) draw_ring_outline(L, i, 0.30f);
        for (int i = 9; i >= 0; i--) draw_dots(L, i);
        for (int i = 0; i < 10; i++) draw_ring_outline(L, i, 0.55f);
        if (app.selected >= 0) draw_ring_outline(L, app.selected, 0.95f);
        hud();
    }
}

// ---------------------------------------------------------------- input
static void pick_ring(int mx, int my) {
    RingLayout L = layout();
    float dx = (mx - L.cx) / (L.s * L.asp * app.zoom);
    float dy = (L.cy - my) / (L.s * app.zoom);
    float d = std::sqrt(dx * dx + dy * dy);
    int hit = -1;
    for (int i = 0; i < 10; i++)
        if (std::fabs(d - ring_mid(i)) <= ring_band(i) * 0.5f + 1.2f) hit = i;
    app.selected = (hit == app.selected) ? -1 : hit;
    app.dirty = true;
}

static void post_action(const char* path, const std::string& body = "") {
    std::string base = app.url;
    size_t p = base.find("/api/state");
    if (p != std::string::npos) base = base.substr(0, p);
    if (body.empty()) {
        std::string b = base + path;
        std::thread([b] { http_post(b, ""); }).detach();
    } else {
        std::string b = base + path;
        std::thread([b, body] { http_post(b, body); }).detach();
    }
}

static void apply_cap(int idx) {
    idx = std::max(0, std::min(CAP_N - 1, idx));
    app.cap_idx = idx;
    int v = CAP_PRESETS[idx];
    post_action("/api/threads", "{\"max\":" + std::to_string(v) + "}");
}

static void on_key(KeySym ks) {
    switch (ks) {
        case XK_q: case XK_Escape: app.running = false; break;
        case XK_p: post_action("/api/pause");   break;
        case XK_s: post_action("/api/resume");  break;
        case XK_t: post_action("/api/stop");    break;
        case XK_c: app.selected = -1; app.dirty = true; break;
        case XK_plus: case XK_equal:  app.zoom = std::min(3.0f, app.zoom + 0.15f); app.dirty = true; break;
        case XK_minus: case XK_underscore: app.zoom = std::max(0.5f, app.zoom - 0.15f); app.dirty = true; break;
        case XK_bracketleft:  apply_cap(app.cap_idx - 1); break;
        case XK_bracketright: apply_cap(app.cap_idx + 1); break;
        default:
            if (ks >= XK_0 && ks <= XK_9) {
                int i = (int)(ks - XK_0);
                app.selected = (app.selected == i) ? -1 : i;
                app.dirty = true;
            }
            break;
    }
}

// ---------------------------------------------------------------- window
static bool init_window() {
    app.dpy = XOpenDisplay(nullptr);
    if (!app.dpy) {
        std::fprintf(stderr, "onion_gl: cannot open X display (DISPLAY=%s)\n",
                     std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "unset");
        return false;
    }
    int screen = DefaultScreen(app.dpy);
    int attribs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                     GLX_BLUE_SIZE, 8, None};
    XVisualInfo* vi = glXChooseVisual(app.dpy, screen, attribs);
    if (!vi) { std::fprintf(stderr, "onion_gl: no suitable GLX visual\n"); return false; }

    Window rootW = RootWindow(app.dpy, screen);
    Colormap cmap = XCreateColormap(app.dpy, rootW, vi->visual, AllocNone);
    XSetWindowAttributes swa{};
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    app.win = XCreateWindow(app.dpy, rootW, 0, 0, gW, gH, 0, vi->depth, InputOutput,
                            vi->visual, CWColormap | CWEventMask, &swa);
    XStoreName(app.dpy, app.win, "Onion GL");
    app.wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app.dpy, app.win, &app.wm_delete, 1);
    XMapWindow(app.dpy, app.win);

    app.ctx = glXCreateContext(app.dpy, vi, nullptr, GL_TRUE);
    XFree(vi);
    if (!app.ctx) { std::fprintf(stderr, "onion_gl: cannot create GLX context\n"); return false; }
    glXMakeCurrent(app.dpy, app.win, app.ctx);

    // vsync if available (both EXT and MESA spellings are common)
    typedef void (*SwapIntervalEXT)(Display*, GLXDrawable, int);
    typedef int  (*SwapIntervalMESA)(unsigned);
    if (auto f = (SwapIntervalEXT)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT"))
        f(app.dpy, app.win, 1);
    else if (auto g = (SwapIntervalMESA)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalMESA"))
        g(1);
    return true;
}

static void process_events() {
    while (XPending(app.dpy)) {
        XEvent e;
        XNextEvent(app.dpy, &e);
        switch (e.type) {
            case Expose: app.dirty = true; break;
            case ConfigureNotify:
                if (e.xconfigure.width != gW || e.xconfigure.height != gH) {
                    gW = e.xconfigure.width; gH = e.xconfigure.height;
                    app.dirty = true;
                }
                break;
            case KeyPress: {
                KeySym ks = XLookupKeysym(&e.xkey, 0);
                on_key(ks);
                break;
            }
            case ButtonPress:
                if (e.xbutton.button == Button1) pick_ring(e.xbutton.x, e.xbutton.y);
                else if (e.xbutton.button == Button4) { app.zoom = std::min(3.0f, app.zoom + 0.12f); app.dirty = true; }
                else if (e.xbutton.button == Button5) { app.zoom = std::max(0.5f, app.zoom - 0.12f); app.dirty = true; }
                break;
            case ClientMessage:
                if ((Atom)e.xclient.data.l[0] == app.wm_delete) app.running = false;
                break;
        }
    }
}

// ---------------------------------------------------------------- polling
static void poll_loop(std::string url, std::atomic<bool>* running) {
    while (running->load()) {
        std::string body = http_get(url);
        if (!body.empty()) {
            try {
                json j = json::parse(body);
                Snapshot s = parse_state(j);
                std::lock_guard<std::mutex> lk(app.mtx);
                app.snap = std::move(s);
                // keep the cap selector aligned with the server's override
                if (app.snap.override_ > 0) {
                    for (int i = 0; i < CAP_N; i++)
                        if (CAP_PRESETS[i] == app.snap.override_) app.cap_idx = i;
                } else app.cap_idx = 0;
                app.dirty = true;
            } catch (...) { /* transient bad body */ }
        } else {
            std::lock_guard<std::mutex> lk(app.mtx);
            if (app.snap.ok) { app.snap = Snapshot(); app.dirty = true; }
        }
        for (int i = 0; i < 20 && running->load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------------------------------------------------------- selftest
static int selftest() {
    // geometry: one dot per node, exact counts
    long long fixture[10] = {9, 17, 95, 788, 15779, 90947, 208499, 0, 0, 0};
    long long total = 0;
    std::vector<float> v;
    for (int i = 0; i < 10; i++) {
        build_dot_positions(i, fixture[i], v);
        if ((long long)(v.size() / 2) != fixture[i]) {
            std::fprintf(stderr, "selftest FAIL: L%d expected %lld dots, got %zu\n",
                         i, fixture[i], v.size() / 2);
            return 1;
        }
        for (float f : v)
            if (!std::isfinite(f)) { std::fprintf(stderr, "selftest FAIL: non-finite L%d\n", i); return 1; }
        total += fixture[i];
    }
    // json parsing: nested schema
    const char* fx = R"({
      "ts": 1, "target_threads": 500, "override": 250,
      "layers": {"L0": 9, "L5": 5},
      "process": {"pid": 42, "alive": true},
      "flags": {"paused": false, "stopped": false},
      "gauges": {"cores": 8, "load1": 1.5, "cpu_busy": 20, "ram_avail_gb": 10, "gpu_pct": null},
      "history": [{"load": 1, "cpu": 2, "ram": 3, "net": 4, "threads": 5}],
      "run": {"seed": "P01", "limit": 100},
      "funnel": {"done": 7, "fail": 1, "live_threads": 12, "node_title": "Initiation",
                 "stage": {"agent": "PLANNER", "ev": "START"},
                 "summary": "funneled=7 kept=33 atomic=2 calls=9 retried=1 errors=0"},
      "progress": {"queued": 3, "agents": {"worker": {"done": 4, "running": 2}},
                   "nodes": [{"title": "n1", "status": "final", "score": 0.9, "kept": 3,
                              "W_done": 4, "W_total": 4, "P": "done", "T": "done", "C": "done", "S": "done"}]}
    })";
    Snapshot s = parse_state(json::parse(fx));
    bool pass = s.ok && s.counts[0] == 9 && s.counts[5] == 5 && s.target_threads == 500 &&
                s.override_ == 250 && s.alive && s.pid == 42 && s.hist.size() == 1 &&
                s.done == 7 && s.kept_total == 33 && s.calls == 9 && s.queued == 3 &&
                s.agents[1] == 4 && s.nodes.size() == 1 && s.nodes[0].status == "final";
    if (!pass) { std::fprintf(stderr, "selftest FAIL: state parsing\n"); return 1; }
    std::printf("selftest OK: 10 rings, %lld dots total; JSON schema parse OK\n", total);
    return 0;
}


// ---------------------------------------------------------------- terminal mode
#ifdef ONION_HAVE_EGL

static volatile sig_atomic_t g_winch = 0;
static void sig_winch(int) { g_winch = 1; }
static void sig_stop(int)  { app.running = false; }

struct TermState {
    struct termios orig{};
    bool raw = false, alt = false, have_px = false;
    int cols = 80, rows = 24, cw = 8, ch = 16;
};
static TermState term;

static void term_query_size() {
    if (g_term_cols > 0) { term.cols = g_term_cols; term.rows = g_term_rows; return; }
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        term.cols = ws.ws_col; term.rows = ws.ws_row;
        if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
            term.cw = std::max(4, ws.ws_xpixel / term.cols);
            term.ch = std::max(8, ws.ws_ypixel / term.rows);
            term.have_px = true;
        }
    }
}

static bool term_enter_raw() {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &term.orig) != 0) return false;
    struct termios t = term.orig;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return false;
    term.raw = true;
    return true;
}

static void write_all(const std::string& s);   // defined below

static void term_leave() {
    if (term.alt) {
        write_all("\x1b[?2026l\x1b[?25h\x1b[?1049l");
        term.alt = false;
    }
    if (term.raw) { tcsetattr(STDIN_FILENO, TCSANOW, &term.orig); term.raw = false; }
}

struct Offscreen {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig cfg = nullptr;
};

static bool egl_start(Offscreen& o, int w, int h) {
    auto plat = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = EGL_NO_DISPLAY;
    if (plat) d = plat(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (d == EGL_NO_DISPLAY) d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY) return false;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(d, &major, &minor)) return false;
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(d, cfg_attr, &o.cfg, 1, &n) || n < 1) return false;
    EGLint pb[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, o.cfg, pb);
    if (s == EGL_NO_SURFACE) return false;
    EGLContext c = eglCreateContext(d, o.cfg, EGL_NO_CONTEXT, nullptr);
    if (c == EGL_NO_CONTEXT) return false;
    if (!eglMakeCurrent(d, s, s, c)) return false;
    o.dpy = d; o.surf = s; o.ctx = c;
    return true;
}

static bool egl_resize(Offscreen& o, int w, int h) {
    eglMakeCurrent(o.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (o.surf != EGL_NO_SURFACE) eglDestroySurface(o.dpy, o.surf);
    EGLint pb[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    o.surf = eglCreatePbufferSurface(o.dpy, o.cfg, pb);
    if (o.surf == EGL_NO_SURFACE) return false;
    return eglMakeCurrent(o.dpy, o.surf, o.surf, o.ctx);
}

static void egl_stop(Offscreen& o) {
    if (o.dpy == EGL_NO_DISPLAY) return;
    eglMakeCurrent(o.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (o.surf != EGL_NO_SURFACE) eglDestroySurface(o.dpy, o.surf);
    if (o.ctx != EGL_NO_CONTEXT) eglDestroyContext(o.dpy, o.ctx);
    eglTerminate(o.dpy);
    o.dpy = EGL_NO_DISPLAY;
}

// framebuffer -> top-down RGBA
static std::vector<uint8_t> term_grab(int w, int h) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> row((size_t)w * 4);
    for (int y = 0; y < h / 2; y++) {
        std::memcpy(row.data(), &px[(size_t)y * w * 4], (size_t)w * 4);
        std::memcpy(&px[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        std::memcpy(&px[(size_t)(h - 1 - y) * w * 4], row.data(), (size_t)w * 4);
    }
    return px;
}

// 216-colour cube + 24-step gray ramp = 240 palette entries
static inline uint8_t sixel_quant(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn < 28) return (uint8_t)(216 + ((int)(r + g + b) / 3) * 23 / 255);
    return (uint8_t)((r * 5 / 255) * 36 + (g * 5 / 255) * 6 + (b * 5 / 255));
}

static std::string sixel_encode(const std::vector<uint8_t>& px, int w, int h) {
    std::string o;
    o.reserve((size_t)w * h / 8 + 8192);
    o += "\x1bPq\"1;1;";
    o += std::to_string(w);
    o += ";";
    o += std::to_string(h);
    char pb[48];
    for (int i = 0; i < 216; i++) {
        int r = (i / 36) * 51, g = ((i / 6) % 6) * 51, b = (i % 6) * 51;
        std::snprintf(pb, sizeof pb, "#%d;2;%d;%d;%d", i,
                      (r * 100 + 127) / 255, (g * 100 + 127) / 255, (b * 100 + 127) / 255);
        o += pb;
    }
    for (int i = 0; i < 24; i++) {
        int l = (i * 255 + 11) / 23;
        std::snprintf(pb, sizeof pb, "#%d;2;%d;%d;%d", 216 + i,
                      (l * 100 + 127) / 255, (l * 100 + 127) / 255, (l * 100 + 127) / 255);
        o += pb;
    }
    std::vector<uint8_t> q((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++)
        q[i] = sixel_quant(px[i * 4], px[i * 4 + 1], px[i * 4 + 2]);

    for (int y0 = 0; y0 < h; y0 += 6) {
        bool used[256] = {false};
        int list[256], n = 0;
        int y1 = std::min(h, y0 + 6);
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++) {
                uint8_t c = q[(size_t)y * w + x];
                if (!used[c]) { used[c] = true; list[n++] = c; }
            }
        for (int ui = 0; ui < n; ui++) {
            o += '#';
            o += std::to_string(list[ui]);
            char run = 0;
            int runN = 0;
            auto flush = [&]() {
                if (!runN) return;
                if (runN >= 4) { o += '!'; o += std::to_string(runN); o += run; }
                else o.append((size_t)runN, run);
                runN = 0;
            };
            for (int x = 0; x < w; x++) {
                int bits = 0;
                for (int k = 0; k < 6; k++) {
                    int y = y0 + k;
                    if (y < h && q[(size_t)y * w + x] == list[ui]) bits |= 1 << k;
                }
                char ch = (char)(63 + bits);
                if (ch == run) runN++;
                else { flush(); run = ch; runN = 1; }
            }
            flush();
            if (ui + 1 < n) o += '$';
        }
        if (y1 < h) o += '-';
    }
    o += "\x1b\\";
    return o;
}

// half-block fallback: 1 cell = (top px, bottom px) as fg/bg + U+2580
static std::string blocks_encode(const std::vector<uint8_t>& px, int w, int h) {
    std::string o;
    o.reserve((size_t)w * h / 2 * 22);
    char buf[80];
    for (int r = 0; r < h / 2; r++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* t = &px[((size_t)(2 * r) * w + x) * 4];
            const uint8_t* b = &px[((size_t)(2 * r + 1) * w + x) * 4];
            std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80",
                          t[0], t[1], t[2], b[0], b[1], b[2]);
            o += buf;
        }
        o += "\x1b[0m";
        if (r + 1 < h / 2) o += "\r\n";
    }
    return o;
}

static void write_all(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(STDOUT_FILENO, s.data() + off, s.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

// sixel through tmux requires DCS passthrough with doubled ESCs
static std::string tmux_wrap(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    o += "\x1bPtmux;\x1b";
    for (char c : s) {
        o += c;
        if (c == 0x1b) o += c;
    }
    o += "\x1b\\";
    return o;
}

// ask for Device Attributes; sixel-capable terminals include attribute 4
// query the real text-area pixel size (CSI 14 t -> CSI 4;H;Wt)
static void term_query_area(int& aw, int& ah) {
    aw = ah = 0;
    const char* q = "\x1b[14t";
    ssize_t w = write(STDOUT_FILENO, q, 5);
    (void)w;
    std::string reply;
    auto t0 = std::chrono::steady_clock::now();
    while (reply.size() < 128 &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.30) {
        struct pollfd pf{};
        pf.fd = STDIN_FILENO;
        pf.events = POLLIN;
        if (poll(&pf, 1, 40) > 0 && (pf.revents & POLLIN)) {
            char b[64];
            ssize_t n = read(STDIN_FILENO, b, sizeof b);
            if (n <= 0) break;
            reply.append(b, (size_t)n);
            if (reply.find('t') != std::string::npos) break;
        }
    }
    size_t p = reply.find("[4;");
    if (p == std::string::npos) return;
    int vals[2] = {0, 0}, vi = 0;
    for (size_t i = p + 3; i < reply.size() && vi < 2; i++) {
        char c = reply[i];
        if (c >= '0' && c <= '9') vals[vi] = vals[vi] * 10 + (c - '0');
        else if (c == ';') vi++;
        else break;
    }
    if (vi == 1 && vals[0] > 50 && vals[1] > 50) { ah = vals[0]; aw = vals[1]; }
}

static bool term_detect_sixel() {
    const char* da1 = "\x1b[c";
    ssize_t w = write(STDOUT_FILENO, da1, 3);
    (void)w;
    std::string reply;
    auto t0 = std::chrono::steady_clock::now();
    while (reply.size() < 256 &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.25) {
        struct pollfd pf{};
        pf.fd = STDIN_FILENO;
        pf.events = POLLIN;
        if (poll(&pf, 1, 40) > 0 && (pf.revents & POLLIN)) {
            char b[128];
            ssize_t n = read(STDIN_FILENO, b, sizeof b);
            if (n <= 0) break;
            reply.append(b, (size_t)n);
            if (reply.find('c') != std::string::npos) break;
        }
    }
    return reply.find(";4;") != std::string::npos || reply.find(";4c") != std::string::npos;
}

static void term_key(unsigned char c) {
    switch (c) {
        case 'q': case 3: case 27: app.running = false; break;
        case 'p': post_action("/api/pause"); break;
        case 's': post_action("/api/resume"); break;
        case 't': post_action("/api/stop"); break;
        case 'c': app.selected = -1; app.dirty = true; break;
        case '+': case '=': app.zoom = std::min(3.0f, app.zoom + 0.15f); app.dirty = true; break;
        case '-': case '_': app.zoom = std::max(0.5f, app.zoom - 0.15f); app.dirty = true; break;
        case '[': apply_cap(app.cap_idx - 1); break;
        case ']': apply_cap(app.cap_idx + 1); break;
        default:
            if (c >= '0' && c <= '9') {
                int i = c - '0';
                app.selected = (app.selected == i) ? -1 : i;
                app.dirty = true;
            }
    }
}

static void term_spawn_if_needed() {
    if (app.spawned || app.no_spawn || !app.local || app.snap.ok) return;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - app.t0).count() < 2.0) return;
    app.spawned = true;
    std::string cmd = "nohup python3 controller.py";
    if (app.port != 8080) cmd += " --port " + std::to_string(app.port);
    if (!app.seed.empty()) cmd += " --seed '" + app.seed + "'";
    if (app.limit > 0) cmd += " --limit " + std::to_string(app.limit);
    cmd += " >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

static int run_term(int mode, bool once) {
    term_query_size();

    struct sigaction sa{};
    sa.sa_handler = sig_winch;
    sigaction(SIGWINCH, &sa, nullptr);
    signal(SIGINT, sig_stop);
    signal(SIGTERM, sig_stop);

    bool tty = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
    if (!once && !tty) {
        std::fprintf(stderr,
            "onion_gl: --term needs a real terminal (stdout/stdin are not TTYs).\n"
            "Run it directly in foot/xterm/wezterm - not through a pipe or an agent output.\n");
        return 1;
    }
    int area_w = 0, area_h = 0;
    if (!once && tty) {
        term_enter_raw();
        if (mode == 0 && !g_term_force && !term_detect_sixel()) {
            mode = 1;   // no sixel support -> readable half-block UI instead of raw bytes
            const char* msg =
                "onion_gl: sixel not detected - using half-block mode (force with --term=sixel)\r\n";
            ssize_t r = write(STDOUT_FILENO, msg, std::strlen(msg));
            (void)r;
        }
        term_query_area(area_w, area_h);
        const char* enter = "\x1b[?1049h\x1b[?25l";
        ssize_t r = write(STDOUT_FILENO, enter, std::strlen(enter));
        (void)r;
        term.alt = true;
    }
    const bool tmux = std::getenv("TMUX") != nullptr;

    auto compute_px = [&](int& pw, int& ph) {
        if (mode == 1) {
            pw = term.cols;
            ph = 2 * term.rows;
            return;
        }
        if (g_term_cols > 0) {              // explicit --term-size override
            pw = std::min(2200, term.cols * term.cw);
            ph = std::min(1400, term.rows * term.ch);
        } else if (area_w > 40 && area_h > 40) {
            // exact text-area pixels from CSI 14t; keep one text row + 4px of
            // margin so the sixel image can never trigger a scroll
            int ch_est = std::max(8, area_h / std::max(1, term.rows));
            pw = std::min(2200, area_w - 4);
            ph = std::min(1400, area_h - ch_est);
        } else {
            pw = std::min(2200, term.cols * term.cw);
            ph = std::min(1400, term.rows * term.ch);
        }
        pw = std::max(80, (int)(pw * g_term_scale));
        ph = std::max(60, (int)(ph * g_term_scale));
    };
    int pw = 0, ph = 0;
    compute_px(pw, ph);
    gW = pw;
    gH = ph;

    Offscreen off;
    if (!egl_start(off, pw, ph)) {
        std::fprintf(stderr, "onion_gl: EGL offscreen context failed "
                             "(terminal mode needs EGL; try the window mode)\n");
        term_leave();
        return 1;
    }

    if (!bake_font(app.f_main, g_font_path ? g_font_path : DEF_FONTS[0], 13.0f))
        std::fprintf(stderr, "onion_gl: font load failed (text will be blank)\n");
    if (!bake_font(app.f_head, g_font_path ? g_font_path : DEF_FONTS[0], 16.0f))
        std::fprintf(stderr, "onion_gl: heading font failed (text will be blank)\n");

    app.dots.assign(10, {});
    app.t0 = std::chrono::steady_clock::now();
    std::thread poller(poll_loop, app.url, &app.running);

    if (once)   // give the first poll a moment so the frame is not empty
        for (int i = 0; i < 40; i++) {
            {
                std::lock_guard<std::mutex> lk(app.mtx);
                if (app.snap.ok) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

    bool first = true;
    while (app.running.load()) {
        if (g_winch) {
            g_winch = 0;
            term_query_size();
            if (tty && !once) term_query_area(area_w, area_h);
            int npw = pw, nph = ph;
            compute_px(npw, nph);
            if (npw != gW || nph != gH) {
                gW = pw = npw; gH = ph = nph;
                if (!egl_resize(off, pw, ph)) break;
                app.dirty = true;
            }
        }
        {
            std::lock_guard<std::mutex> lk(app.mtx);
            if (app.snap.ok)
                for (int i = 0; i < 10; i++)
                    if (app.snap.counts[i] != app.built[i]) {
                        build_dot_positions(i, app.snap.counts[i], app.dots[i]);
                        app.built[i] = app.snap.counts[i];
                        app.dirty = true;
                    }
            term_spawn_if_needed();
        }
        if (tty && !once) {
            struct pollfd pf{};
            pf.fd = STDIN_FILENO;
            pf.events = POLLIN;
            if (poll(&pf, 1, 120) > 0 && (pf.revents & POLLIN)) {
                unsigned char buf[64];
                ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
                for (ssize_t k = 0; k < n; k++) term_key(buf[k]);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (app.dirty || first) {
            app.dirty = false;
            render();
            std::vector<uint8_t> frame = term_grab(gW, gH);
            std::string payload = (mode == 0) ? sixel_encode(frame, gW, gH)
                                              : blocks_encode(frame, gW, gH);
            if (once) {
                write_all(payload);
                break;
            }
            std::string out;
            std::string body = (mode == 0 && tmux) ? tmux_wrap(payload) : payload;
            out.reserve(body.size() + 32);
            out += "\x1b[?2026h\x1b[H";   // synchronized update + home
            out += body;
            out += "\x1b[J\x1b[?2026l";   // clear leftovers + end sync
            write_all(out);
            first = false;
        }
    }
    app.running = false;
    poller.join();
    egl_stop(off);
    term_leave();
    return 0;
}
#endif // ONION_HAVE_EGL

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    std::string url = std::getenv("ONION_TUI_URL") ? std::getenv("ONION_TUI_URL") : "";
    int port = 8080;
    if (const char* e = std::getenv("ONION_PORT")) port = std::atoi(e);
    bool url_given = !url.empty(), port_given = false;
    const char* font_path = nullptr;
    std::string font_path_storage;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        // option value: supports both "--opt value" and "--opt=value"
        auto value = [&](const char* name, std::string& out) -> bool {
            const std::string eq = std::string(name) + "=";
            if (a == name && i + 1 < argc) { out = argv[++i]; return true; }
            if (a.rfind(eq, 0) == 0) { out = a.substr(eq.size()); return true; }
            return false;
        };
        std::string v;
        if (a == "--selftest") return selftest();
        else if (a == "--term") { g_term_mode = 0; }              // auto (sixel if detected, else blocks)
        else if (a == "--term=blocks") { g_term_mode = 1; g_term_force = true; }
        else if (a == "--term=sixel") { g_term_mode = 0; g_term_force = true; }  // force sixel
        else if (a == "--term-once") { g_term_mode = 0; g_term_once = true; }
        else if (value("--term-scale", v) || value("--scale", v)) {
            g_term_scale = (float)std::atof(v.c_str());
            if (g_term_scale <= 0.f) g_term_scale = 1.0f;   // bad value -> default
            if (g_term_scale < 0.3f) g_term_scale = 0.3f;
            if (g_term_scale > 1.0f) g_term_scale = 1.0f;
        }
        else if (value("--term-size", v)) {
            int c = 0, r = 0;
            if (std::sscanf(v.c_str(), "%dx%d", &c, &r) == 2 && c > 10 && r > 5) {
                g_term_cols = c; g_term_rows = r;
            }
        }
        else if (value("--url", v)) { url = v; url_given = true; }
        else if (value("--port", v)) { port = std::atoi(v.c_str()); port_given = true; }
        else if (a == "--no-spawn") app.no_spawn = true;
        else if (value("--seed", v)) app.seed = v;
        else if (value("--limit", v)) app.limit = std::atoi(v.c_str());
        else if (value("--width", v)) gW = std::atoi(v.c_str());
        else if (value("--height", v)) gH = std::atoi(v.c_str());
        else if (value("--font", v)) {
            font_path_storage = v;
            font_path = font_path_storage.c_str();
        }
        else if (a == "-h" || a == "--help") {
            std::printf("usage: onion_gl [--url URL] [--port N] [--no-spawn] [--seed S] [--limit N]\n"
                        "                [--width W] [--height H] [--font TTF] [--selftest]\n"
                        "                [--term[=sixel|blocks]] [--term-once] [--term-size COLSxROWS]\n"
                        "                [--term-scale F | --scale F (0.3-1.0, default 1.0)]\n"
                        "                any --opt=value form is accepted too\n"
                        "                --term auto-detects sixel and falls back to half-blocks\n");
            return 0;
        }
    }
    (void)port_given;
    if (!font_path)
        for (const char* f : DEF_FONTS)
            if (access(f, R_OK) == 0) { font_path = f; break; }
    g_font_path = font_path;
    if (!url_given) url = "http://127.0.0.1:" + std::to_string(port) + "/api/state";
    app.url = url;
    app.port = port;
    app.local = url.find("127.0.0.1") != std::string::npos ||
                url.find("localhost") != std::string::npos;

#ifdef ONION_HAVE_EGL
    if (g_term_mode >= 0) return run_term(g_term_mode, g_term_once);
#else
    if (g_term_mode >= 0) {
        std::fprintf(stderr, "onion_gl: built without EGL - terminal mode unavailable\n");
        return 1;
    }
#endif

    if (!init_window()) return 1;

    if (!bake_font(app.f_main, font_path, 14.0f))
        std::fprintf(stderr, "onion_gl: font not found at %s (text will be blank)\n", font_path);
    if (!bake_font(app.f_head, font_path, 18.0f))
        std::fprintf(stderr, "onion_gl: heading font failed (text will be blank)\n");

    app.dots.assign(10, {});
    app.t0 = std::chrono::steady_clock::now();
    std::thread poller(poll_loop, url, &app.running);

    while (app.running.load()) {
        process_events();
        {
            std::lock_guard<std::mutex> lk(app.mtx);
            if (app.snap.ok)
                for (int i = 0; i < 10; i++)
                    if (app.snap.counts[i] != app.built[i]) {
                        build_dot_positions(i, app.snap.counts[i], app.dots[i]);
                        app.built[i] = app.snap.counts[i];
                        app.dirty = true;
                    }
            if (!app.spawned && !app.no_spawn && app.local && !app.snap.ok &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - app.t0).count() > 2.0) {
                app.spawned = true;
                std::string cmd = "nohup python3 controller.py";
                if (port != 8080) cmd += " --port " + std::to_string(port);
                if (!app.seed.empty()) cmd += " --seed '" + app.seed + "'";
                if (app.limit > 0) cmd += " --limit " + std::to_string(app.limit);
                cmd += " >/dev/null 2>&1 &";
                std::system(cmd.c_str());
            }
        }
        if (app.dirty) {
            app.dirty = false;
            render();
            glXSwapBuffers(app.dpy, app.win);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    app.running = false;
    poller.join();
    if (app.ctx) { glXMakeCurrent(app.dpy, None, nullptr); glXDestroyContext(app.dpy, app.ctx); }
    if (app.win) XDestroyWindow(app.dpy, app.win);
    if (app.dpy) XCloseDisplay(app.dpy);
    return 0;
}
