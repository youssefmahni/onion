#include "depot_io.hpp"

#ifdef DEPOT_HAVE_OPENCL
#include "depot_gpu.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <functional>
#include <deque>
#include <future>
#include <limits>
#include <thread>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#include <unistd.h>

#include "depot_core.hpp"

using depot::Row;
using std::string;
using std::vector;

// Key-driven provider routing: a DEPOT_API_URL override always wins; otherwise
// the key decides — OpenRouter keys ("sk-or-...") go to OpenRouter, anything
// else to DeepSeek. Dropping a new key into the env/.onion_key "just works".
static string api_url_for(const string& key) {
    if (const char* env = std::getenv("DEPOT_API_URL"); env && *env) return string(env);
    if (key.rfind("sk-or", 0) == 0)
        return "https://openrouter.ai/api/v1/chat/completions";
    return "https://api.deepseek.com/chat/completions";
}
static string curl_post(const string& payload, const string& key);  // defined below (llm section)

// ---------------- file helpers ----------------
static string join_path(const string& root, const string& fn) {
    return (root.empty() || root == ".") ? fn : root + "/" + fn;
}

static string esc_csv_field(const string& s) {
    if (s.find_first_of(",\"\n") == string::npos) return s;
    string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o += c; }
    return o + "\"";
}

static vector<Row> load_rows(const string& fn) {
    vector<Row> out;
    std::ifstream f(fn);
    string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        if (depot::trim(line).empty()) continue;
        auto c = depot::split_csv(line);
        if (c.size() == 2)          // 2-column file (level_00: ID,Phase)
            out.push_back({depot::trim(c[0]), depot::trim(c[0]), depot::trim(c[1])});
        else if (c.size() >= 3)
            out.push_back({depot::trim(c[0]), depot::trim(c[1]), depot::trim(c[2])});
    }
    return out;
}

static void write_rows(const string& fn, const vector<string>& header, const vector<Row>& rows) {
    std::ofstream f(fn);
    for (size_t i = 0; i < header.size(); ++i) f << (i ? "," : "") << header[i];
    f << "\n";
    for (const auto& r : rows) {
        if (header.size() == 2) f << esc_csv_field(r.id) << "," << esc_csv_field(r.title) << "\n";
        else f << esc_csv_field(r.id) << "," << esc_csv_field(r.path) << "," << esc_csv_field(r.title) << "\n";
    }
}

// ---------------- ingest command ----------------
struct IngestReport {
    int added = 0, skipped = 0, merged = 0, candidate = 0, flagged = 0, no_td = 0;
};

int depot::run_ingest(const string& input, bool dry, const string& restrict_prefix,
                      const string& root, bool use_gpu) {
    auto l4 = load_rows(join_path(root, "Onion - level_04.csv"));
    auto l5 = load_rows(join_path(root, "Onion - level_05.csv"));
    auto l6 = load_rows(join_path(root, "Onion - level_06.csv"));

    // decision registry: title -> subs
    std::unordered_map<string, string> decisions;
    string line;
    {
        std::ifstream f(join_path(root, "expand_decisions.csv"));
        std::getline(f, line);
        while (std::getline(f, line)) {
            auto c = depot::split_csv(line);
            if (c.size() < 5) continue;
            if (c[2] == "expand" && !c[3].empty()) decisions[depot::trim(c[0])] = c[3];
        }
    }

    std::unordered_map<string, int> title_freq;
    for (const auto& r : l5) title_freq[r.title]++;
    std::unordered_map<string, string> canon_map;
    for (const auto& kv : title_freq) canon_map[kv.first] = depot::canonical(kv.first);
    std::unordered_map<string, string> canon_of;
    for (const auto& r : l5) canon_of[r.path] = depot::canonical(r.title);

    // fingerprint index: canonical key -> set of L5 paths
    std::unordered_map<string, vector<string>> index;
    for (const auto& r : l5) index[depot::canonical(r.title)].push_back(r.path);

#ifdef DEPOT_HAVE_OPENCL
    std::unique_ptr<depot::gpu::SimIndex> gspell_idx;
    std::vector<string> gspell_titles, gspell_keys;
    bool gspell_gpu = false;
    if (use_gpu) {
        gspell_titles.reserve(canon_map.size());
        gspell_keys.reserve(canon_map.size());
        for (const auto& kv : canon_map) {
            gspell_titles.push_back(kv.first);
            gspell_keys.push_back(kv.second);
        }
        gspell_idx = std::make_unique<depot::gpu::SimIndex>();
        string gerr;
        if (gspell_idx->build(gspell_keys, gerr)) {
            gspell_gpu = true;
            std::cout << "[gpu] global-spelling similarity on OpenCL ("
                      << gspell_keys.size() << " candidates on "
                      << depot::gpu::sim_device_name() << ")\n";
        } else {
            std::cerr << "[gpu] disabled, CPU fallback: " << gerr << "\n";
            gspell_idx.reset();
        }
    }
#else
    (void)use_gpu;
#endif

    int tk_n = depot::next_tk(l5, l6);
    vector<Row> new5, new6;
    vector<string> aliases;
    IngestReport rep;
    std::ifstream in(input);
    if (!in) { std::cerr << "cannot open " << input << "\n"; return 1; }

    auto same_td_dup = [&](const string& key, const string& tdpath) -> bool {
        auto it = index.find(key);
        if (it == index.end()) return false;
        for (const auto& p : it->second)
            if (depot::td_prefix(p) == depot::td_prefix(tdpath)) return true;
        return false;
    };
    auto depot_similar_same_td = [&](const string& key, const string& tdpath)
        -> std::pair<string, double> {
        string best; double bests = 0.0;
        for (const auto& r : l5) {
            if (depot::td_prefix(r.path) != depot::td_prefix(tdpath)) continue;
            double s = depot::sim_keys(canon_of[r.path], key);
            if (s > bests) { bests = s; best = r.title; }
        }
        return {best, bests};
    };
    auto global_spelling = [&](const string& key, const string& incoming)
        -> std::pair<string, double> {
        string best; double bests = 0.0; int bestn = -1;
#ifdef DEPOT_HAVE_OPENCL
        if (gspell_gpu) {
            std::vector<double> sc;
            string gerr;
            if (gspell_idx->scores({key}, sc, gerr) && sc.size() == gspell_titles.size()) {
                for (size_t j = 0; j < sc.size(); ++j) {
                    double s = sc[j];
                    if (s < 0.85) continue;
                    const string& t = gspell_titles[j];
                    int n = title_freq[t];
                    bool better = s > bests ||
                        (s == bests && (n > bestn ||
                         (n == bestn && t != incoming &&
                          (best.empty() || t < best))));
                    if (better) { best = t; bests = s; bestn = n; }
                }
                return {best, bests};
            }
            gspell_gpu = false;   // kernel issue mid-run -> CPU from here on
            std::cerr << "[gpu] kernel error, CPU fallback: " << gerr << "\n";
        }
#endif
        for (const auto& kv : canon_map) {
            double s = depot::sim_keys(kv.second, key);
            if (s < 0.85) continue;
            int n = title_freq[kv.first];
            bool better = s > bests ||
                (s == bests && (n > bestn ||
                 (n == bestn && kv.first != incoming &&
                  (best.empty() || kv.first < best))));
            if (better) {
                best = kv.first; bests = s; bestn = n;
            }
        }
        return {best, bests};
    };
    auto find_td = [&](const string& t) -> std::pair<string, string> {
        for (const auto& r : l4)
            if ((restrict_prefix.empty() || r.path.rfind(restrict_prefix, 0) == 0) && r.title == t)
                return {r.path, "exact"};
        string best; double bests = 0.0;
        for (const auto& r : l4) {
            if (!restrict_prefix.empty() && r.path.rfind(restrict_prefix, 0) != 0) continue;
            double s = depot::lcs_ratio(depot::lower(r.title), depot::lower(t));
            if (s > bests) { bests = s; best = r.path; }
        }
        if (bests >= 0.80) return {best, "fuzzy"};
        return {"", "no-match"};
    };

    while (std::getline(in, line)) {
        if (depot::trim(line).empty()) continue;
        std::stringstream ss(line);
        string td, title, subs;
        std::getline(ss, td, '\t');
        std::getline(ss, title, '\t');
        std::getline(ss, subs, '\t');
        td = depot::trim(td); title = depot::trim(title);
        if (title.empty()) continue;
        auto flags = depot::style_flags(title);
        auto [tdpath, _how] = find_td(td);
        (void)_how;
        if (tdpath.empty()) { rep.no_td++; continue; }
        string key = depot::canonical(title);
        if (same_td_dup(key, tdpath)) { rep.skipped++; continue; }
        auto [near, score] = depot_similar_same_td(key, tdpath);
        string verdict = "new";
        if (!near.empty()) {
            if (score >= 0.85) {
                rep.merged++;
                aliases.push_back(near + "\t" + title + "\tauto-merge");
                continue;
            }
            if (score >= 0.70) { verdict = "candidate"; rep.candidate++; }
        }
        auto [gspell, gscore] = global_spelling(key, title);
        if (!gspell.empty() && gspell != title) {
            aliases.push_back(gspell + "\t" + title + "\tspelling-normalized");
            title = gspell;
            key = depot::canonical(title);
        }
        if (same_td_dup(key, tdpath)) { rep.skipped++; continue; }
        char idbuf[16];
        std::snprintf(idbuf, sizeof idbuf, "TK%05d", tk_n);
        string id = idbuf, path = tdpath + "-" + id;
        new5.push_back({id, path, title});
        index[key].push_back(path);
        tk_n++;
        if (verdict == "new") rep.added++;

        if (!flags.empty()) rep.flagged++;
        auto use = decisions.find(title);
        string chosen;
        if (!subs.empty()) chosen = subs;
        else if (use != decisions.end()) chosen = use->second;
        for (const auto& s : depot::split_pipe(chosen)) {
            std::snprintf(idbuf, sizeof idbuf, "TK%05d", tk_n);
            string cid = idbuf;
            new6.push_back({cid, path + "-" + cid, s});
            tk_n++;
        }
    }

    if (!dry) {
        vector<Row> l5w = l5, l6w = l6;
        l5w.insert(l5w.end(), new5.begin(), new5.end());
        l6w.insert(l6w.end(), new6.begin(), new6.end());
        write_rows(join_path(root, "Onion - level_05.csv"), {"ID", "ID-L5", "Title"}, l5w);
        write_rows(join_path(root, "Onion - level_06.csv"), {"ID", "ID-L6", "Title"}, l6w);
        std::ofstream al(join_path(root, "title_aliases.tsv"), std::ios::app);
        for (const auto& a : aliases) al << a << "\n";
        std::unordered_set<string> ids, paths;
        int dups = 0;
        for (const auto& r : l5w) { if (!ids.insert(r.id).second) dups++; paths.insert(r.path); }
        for (const auto& r : l6w) { if (!ids.insert(r.id).second) dups++; }
        int orphans = 0;
        for (const auto& r : l6w) {
            string parent = r.path.substr(0, r.path.rfind('-'));
            if (!paths.count(parent)) orphans++;
        }
        std::cout << "added=" << rep.added << " skipped=" << rep.skipped
                  << " merged=" << rep.merged << " candidate=" << rep.candidate
                  << " flagged=" << rep.flagged << " no_td=" << rep.no_td
                  << " l5=" << l5w.size() << " l6=" << l6w.size()
                  << " dup_ids=" << dups << " orphans=" << orphans << "\n";
    } else {
        std::cout << "DRY added=" << rep.added << " skipped=" << rep.skipped
                  << " merged=" << rep.merged << " candidate=" << rep.candidate
                  << " flagged=" << rep.flagged << " no_td=" << rep.no_td << "\n";
    }
    for (const auto& a : aliases) std::cout << "ALIAS\t" << a << "\n";
    return 0;
}

// ---------------- funnel command ----------------
// Four-agent pipeline per node:
//   Planner  -> aspects of the node (roadmap)
//   Workers  -> parallel per-aspect candidate generation (top ~500 merged)
//   Trimmers -> keep best 400 -> 300 (worker-style narrowing)
//   Critique -> validates, scores confidence, flags hallucinations + missing items
//   Synthesizer -> final balanced set (3-100) with atomic flags, resolves conflicts
// Low-score nodes are re-queued once with the critique issues, so a miss in one
// round is fixed by the next.
struct FunnelNode {
    string layer, path, title;
};

static void append_funnel_state(const string& file, const string& path, int kept, int atomic,
                                int retries, double score, bool dry) {
    if (dry) return;
    std::ofstream f(file, std::ios::app);
    f << path << "\t" << kept << "\t" << atomic << "\t" << retries << "\t" << score << "\t"
      << (long)time(nullptr) << "\n";
}

static int path_segments(const string& path) {
    int cnt = 1;
    for (char c : path) if (c == '-') cnt++;
    return cnt;
}

static string level_file_for(const string& root, int segments) {
    return join_path(root, string("Onion - level_") + (segments < 10 ? "0" : "") +
                            std::to_string(segments) + ".csv");
}

// Provider-aware reasoning knob (must match the endpoint the key routes to):
// DeepSeek only honors its own "reasoning_effort" field and otherwise burns
// the token budget on reasoning; OpenRouter uses the OpenAI-style object.
// An explicit DEPOT_API_URL override decides FIRST, then the key prefix.
static string reasoning_param(const string& key) {
    if (const char* env = std::getenv("DEPOT_API_URL"); env && *env) {
        string u = env;
        if (u.find("deepseek") != string::npos) return "\"reasoning_effort\":\"none\"";
        if (u.find("openrouter") != string::npos) return "\"reasoning\":{\"effort\":\"low\"}";
    }
    return key.rfind("sk-or", 0) == 0 ? "\"reasoning\":{\"effort\":\"low\"}"
                                      : "\"reasoning_effort\":\"none\"";
}

static string funnel_payload(const string& model, const string& user, int max_tokens,
                             const string& key) {
    string sys = "You are a strict knowledge-curation assistant for a hierarchical "
                 "work-breakdown knowledge base that powers AI agents. Do not reason out "
                 "loud and do not write any explanation — answer ONLY with the JSON object "
                 "requested, nothing else.";
    return "{\"model\":\"" + model + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" +
           depot::json_escape(sys) + "\"},{\"role\":\"user\",\"content\":\"" +
           depot::json_escape(user) + "\"}],\"max_tokens\":" + std::to_string(max_tokens) +
           ",\"temperature\":0,\"stream\":false," + reasoning_param(key) + "}";
}

int depot::run_funnel(const string& branch, int limit, int concurrency, bool dry, bool force,
                      const string& model, const string& key, const string& root, AskFn raw_ask,
                      const string& seed_path, int rounds, bool all_branches, bool deep) {
    static std::atomic<int> body_seq{0};
    auto ask = [&](const string& payload) -> string {
        string b = raw_ask ? raw_ask(payload) : curl_post(payload, key);
        if (std::getenv("DEPOT_DUMP_BODIES")) {
            char fn[64];
            std::snprintf(fn, sizeof fn, "/tmp/depot_body_%03d.json", body_seq.fetch_add(1));
            std::ofstream db(fn);
            db << payload << "\n===RESPONSE===\n" << b;
        }
        return b;
    };
    const vector<string> files = {
        "Onion - level_00.csv","Onion - level_01.csv","Onion - level_02.csv","Onion - level_03.csv",
        "Onion - level_04.csv","Onion - level_05.csv","Onion - level_06.csv","Onion - level_07.csv",
        "Onion - level_08.csv","Onion - level_09.csv"};

    std::unordered_map<string, FunnelNode> by_path;
    for (size_t li = 0; li < files.size(); ++li) {
        std::ifstream f(join_path(root, files[li]));
        string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (depot::trim(line).empty()) continue;
            auto c = depot::split_csv(line);
            if (c.size() < 2) continue;
            FunnelNode n;
            n.layer = "L" + std::to_string(li);
            if (li == 0) { n.path = depot::trim(c[0]); n.title = depot::trim(c[1]); }
            else { if (c.size() < 3) continue; n.path = depot::trim(c[1]); n.title = depot::trim(c[2]); }
            if (!n.path.empty()) by_path[n.path] = n;
        }
    }
    std::unordered_map<string, vector<string>> children_of;
    for (auto& kv : by_path) {
        size_t d = kv.first.rfind('-');
        if (d != string::npos) children_of[kv.first.substr(0, d)].push_back(kv.first);
    }

    std::unordered_set<string> done;
    std::unordered_map<string, int> state_retries;
    {
        std::ifstream f(join_path(root, "funnel_state.tsv"));
        string line;
        while (std::getline(f, line)) {
            if (depot::trim(line).empty()) continue;
            size_t t1 = line.find('\t');
            if (t1 == string::npos) continue;
            string path = line.substr(0, t1);
            done.insert(path);

            // optional retries column (4th, index 3)
            size_t t2 = line.find('\t', t1 + 1);
            size_t t3 = t2 == string::npos ? string::npos : line.find('\t', t2 + 1);
            size_t t4 = t3 == string::npos ? string::npos : line.find('\t', t3 + 1);
            if (t4 != string::npos) {
                string ret_s = line.substr(t3 + 1, t4 - t3 - 1);
                state_retries[path] = std::atoi(ret_s.c_str());
            }
        }
    }

    string eff_branch = all_branches ? "" : branch;
    vector<string> seeds;
    if (!seed_path.empty()) {
        if (by_path.count(seed_path)) seeds.push_back(seed_path);
    } else {
        for (auto& kv : by_path)
            if (kv.second.layer == "L4" &&
                (eff_branch.empty() || kv.first.rfind(eff_branch, 0) == 0))
                seeds.push_back(kv.first);
    }
    std::sort(seeds.begin(), seeds.end());
    std::deque<string> queue;  // seeded per round inside the rounds loop

    int tk_n = 0;
    for (auto& kv : by_path) {
        size_t p = kv.first.rfind("-TK");
        if (p != string::npos) tk_n = std::max(tk_n, std::atoi(kv.first.c_str() + p + 3));
    }
    std::atomic<int> tk_next{tk_n + 1};

    std::mutex m;
    std::unordered_map<string, string> retry_issues;
    std::unordered_map<string, int> retry_count;
    std::atomic<int> funneled{0}, kept_total{0}, atomic_total{0}, call_total{0}, retried{0}, errors{0};
    int budget = limit > 0 ? limit : std::numeric_limits<int>::max();
    const double RETRY_SCORE = 0.7;

    auto body_failed = [](const string& body) {
        // A valid chat-completions body always carries "choices"; anything
        // without it (proxy 502/503 HTML pages, auth errors, malformed JSON)
        // must not masquerade as success.
        return body.empty() || body.find("\"choices\"") == string::npos;
    };
    auto agent_log = [&](const string& path, const string& stage, const string& ev) {
        std::lock_guard<std::mutex> lk(m);
        std::cout << "AGENT\t" << path << "\t" << stage << "\t" << ev << std::endl;
    };
    auto on_failure = [&](const string& path, int kept, int atom, const string& stage) {
        std::lock_guard<std::mutex> lk(m);
        errors++;
        std::cout << "AGENT\t" << path << "\t" << stage << "\tFAIL" << std::endl;
        std::cout << "NODE_FAIL\t" << path << "\tempty-or-error API body" << std::endl;
        if (retry_count[path] < 1) {
            retry_count[path]++;
            queue.push_back(path);
        } else {
            int retries = 1;
            auto ri = state_retries.find(path);
            if (ri != state_retries.end()) retries = ri->second + retry_count[path];
            append_funnel_state(join_path(root, "funnel_state.tsv"), path, kept, atom,
                                retries, 0.0, dry);
            done.insert(path);
        }
    };

    auto work = [&](const string& path) {
        FunnelNode node;
        vector<string> existing;
        {
            std::lock_guard<std::mutex> lk(m);
            auto it = by_path.find(path);
            if (it == by_path.end()) return;
            node = it->second;
            for (auto& c : children_of[path]) existing.push_back(by_path[c].title);
        }
        int segs = path_segments(path);
        if (segs >= 10) {
            // L9 hard cap: never expand, but settle it so later rounds don't
            // re-queue, re-count, and re-log the same node forever.
            std::lock_guard<std::mutex> lk(m);
            if (!done.count(path)) {
                int retries = 0;
                auto ri = state_retries.find(path);
                if (ri != state_retries.end()) retries = ri->second;
                append_funnel_state(join_path(root, "funnel_state.tsv"), path, 0, 0,
                                    retries, 1.0, dry);
                done.insert(path);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m);
            std::cout << "NODE_START\t" << path << "\t" << node.title << std::endl;
        }
        string exj;
        for (size_t i = 0; i < existing.size(); ++i) exj += (i ? "|" : "") + existing[i];

        auto round_user = [&](const string& stage) {
            return "Node: " + node.layer + " " + node.path + "\nTitle: " + node.title + "\n" + stage;
        };

        // ---- Planner Agent: aspects roadmap ----
        string issues_ctx;
        {
            std::lock_guard<std::mutex> lk(m);
            auto qi = retry_issues.find(path);
            if (qi != retry_issues.end() && !qi->second.empty())
                issues_ctx = "Previous critique issues you MUST address: " + qi->second + "\n";
        }
        string p_user = round_user(issues_ctx +
            "Existing children (do NOT repeat any of these): " + exj + "\n"
            "Plan the knowledge coverage for this node: list 4-8 aspects. Every aspect must be "
            "a dimension OF THE NODE ITSELF (things it directly contains or decomposes into) — "
            "never related topics from other domains. Return JSON {\"items\": [ aspects ]}.");
        agent_log(path, "PLANNER", "START");
        call_total++;
        string p_body = ask(funnel_payload(model, p_user, 6000, key));
        if (body_failed(p_body)) { on_failure(path, 0, 0, "PLANNER"); return; }
        auto aspects = depot::parse_funnel_r1(p_body);
        if (aspects.size() > 8) aspects.resize(8);
        if (aspects.empty()) aspects.push_back("General coverage");
        agent_log(path, "PLANNER", "DONE aspects=" + std::to_string(aspects.size()));

        // ---- Worker Agents: parallel per-aspect candidate generation ----
        vector<vector<string>> aspect_cands(aspects.size());
        {
            std::vector<std::future<void>> futs;
            for (size_t ai = 0; ai < aspects.size(); ++ai) {
                futs.push_back(std::async(std::launch::async, [&, ai] {
                    agent_log(path, "WORKER", "START " + depot::sanitize_tsv(aspects[ai]));
                    string w_user = round_user(
                        "Aspect: " + aspects[ai] + "\nExisting children (do NOT repeat): " + exj + "\n"
                        "List the top 100 child items of this node for this aspect in an AI-agent "
                        "knowledge base. Every item MUST be a direct child of \"" + node.title +
                        "\" within its exact scope — do NOT include related topics, industry "
                        "classifications, or the node itself. Title Case noun phrases, 2-5 words, "
                        "no duplicates, no questions, no emoji. "
                        "Return JSON {\"items\": [ ... up to 100 ... ]}.");
                    call_total++;
                    aspect_cands[ai] = depot::parse_funnel_r1(ask(funnel_payload(model, w_user, 6000, key)));
                    agent_log(path, "WORKER", "DONE " + depot::sanitize_tsv(aspects[ai]) +
                              " items=" + std::to_string(aspect_cands[ai].size()));
                }));
            }
            for (auto& f : futs) f.get();
        }
        std::unordered_set<string> seen_c;
        vector<string> cands;
        for (auto& lst : aspect_cands) {
            for (auto& t : lst) {
                if (cands.size() >= 500) break;
                if (seen_c.insert(depot::canonical(t)).second) cands.push_back(t);
            }
        }

        // ---- Trimmer rounds (worker-style narrowing): 400 -> 300 ----
        int trim_failures = 0;
        auto trim_round = [&](vector<string>& list, size_t keep) {
            if (list.empty() || list.size() <= keep) return;
            string joined;
            for (size_t i = 0; i < list.size(); ++i) joined += (i ? "|" : "") + list[i];
            string u = round_user(
                "From these candidates keep only the best " + std::to_string(keep) +
                " — drop the weakest, you may re-rank. Return the titles VERBATIM, "
                "do not rephrase them. Candidates:\n" + joined +
                "\nReturn JSON {\"items\": [ ... exactly " + std::to_string(keep) + " ... ]}.");
            agent_log(path, "TRIMMER", "START " + std::to_string(keep));
            call_total++;
            string t_body = ask(funnel_payload(model, u, 6000, key));
            if (body_failed(t_body)) {
                trim_failures++;
                agent_log(path, "TRIMMER", "FAIL " + std::to_string(keep));
                return;  // keep the old list; both rounds failing = node failure below
            }
            auto kept = depot::parse_funnel_r1(t_body);
            if (kept.size() > keep) kept.resize(keep);
            if (!kept.empty()) list = kept;
            agent_log(path, "TRIMMER", "DONE " + std::to_string(keep) +
                      " items=" + std::to_string(list.size()));
        };
        // ---- local near-duplicate sweep (GPU/CPU copy-cat cleaner) ----
        // Drops near-identical candidates (sim >= 0.95, first occurrence kept)
        // BEFORE the LLM trimmer, so the AI works on a cleaner pool.
        if (cands.size() >= 2) {
            std::vector<std::string> canon;
            canon.reserve(cands.size());
            for (const auto& t : cands) canon.push_back(depot::canonical(t));
            std::vector<char> drop(canon.size(), 0);
            bool swept = false;
#ifdef DEPOT_HAVE_OPENCL
            {
                depot::gpu::SimIndex idx;
                std::string gerr;
                std::vector<double> m;
                if (idx.build(canon, gerr) && idx.scores(canon, m, gerr) &&
                    m.size() == canon.size() * canon.size()) {
                    for (size_t j = 1; j < canon.size(); ++j)
                        for (size_t i = 0; i < j; ++i)
                            if (!drop[i] && m[i * canon.size() + j] >= 0.95) { drop[j] = 1; break; }
                    swept = true;
                }
            }
#endif
            if (!swept) {
                for (size_t j = 1; j < canon.size(); ++j)
                    for (size_t i = 0; i < j; ++i)
                        if (!drop[i] && depot::sim_keys(canon[i], canon[j]) >= 0.95) {
                            drop[j] = 1;
                            break;
                        }
            }
            size_t w = 0, removed = 0;
            for (size_t i = 0; i < cands.size(); ++i) {
                if (drop[i]) { ++removed; continue; }
                if (w != i) cands[w] = std::move(cands[i]);
                ++w;
            }
            cands.resize(w);
            if (removed)
                agent_log(path, "DEDUP", "DONE removed " + std::to_string(removed));
        }

        trim_round(cands, 400);
        trim_round(cands, 300);
        if (trim_failures >= 2) { on_failure(path, 0, 0, "TRIMMER"); return; }

        string joined;
        for (size_t i = 0; i < cands.size(); ++i) joined += (i ? "|" : "") + cands[i];

        // ---- Critique Agent: validate, score, flag ----
        string c_user = round_user(
            "Critique this candidate set for the node. This is a RAW CANDIDATE POOL meant to be "
            "filtered: score it on how many candidates are usable direct children of \"" +
            node.title + "\" (precision), not on whether every candidate is perfect. Check: "
            "(1) hallucinations or off-topic items, (2) duplicates, (3) important missing "
            "categories, (4) weak or vague items, (5) compliance (Title Case, no questions, no "
            "emoji). Candidates:\n" + joined +
            "\nReturn JSON {\"score\": 0.0-1.0 usable-candidate ratio, \"remove\": [ items to drop ], "
            "\"add\": [ missing items to include ], \"issues\": \"short list of problems\"}.");
        agent_log(path, "CRITIQUE", "START");
        call_total++;
        string c_body = ask(funnel_payload(model, c_user, 6000, key));
        if (body_failed(c_body)) { on_failure(path, 0, 0, "CRITIQUE"); return; }
        auto crit = depot::parse_funnel_c(c_body);
        agent_log(path, "CRITIQUE", "DONE score=" + std::to_string(crit.score) +
                  " bodylen=" + std::to_string(c_body.size()));

        // ---- Synthesizer Agent: final balanced set ----
        string crit_txt = "score=" + std::to_string(crit.score) +
                          ", remove: " + [&]{ string o; for (auto& t : crit.remove) o += t + ";"; return o; }() +
                          ", add: " + [&]{ string o; for (auto& t : crit.add) o += t + ";"; return o; }() +
                          ", issues: " + crit.issues;
        string s_user = round_user(
            "From these candidates select the final children set for this node. Incorporate the "
            "critique: drop hallucinated or weak items, include the missing items you judge "
            "valid, resolve any conflicts. You decide how many to keep (between 3 and 100). For "
            "each kept item return {\"title\": \"...\", \"atomic\": true if it cannot be "
            "decomposed into simpler items, false if it should nest further}. Candidates:\n" +
            joined + "\nCritique: " + crit_txt +
            "\nReturn JSON {\"items\": [ {\"title\": ..., \"atomic\": ...}, ... ]}.");
        agent_log(path, "SYNTHESIZER", "START");
        call_total++;
        string s_body = ask(funnel_payload(model, s_user, 6000, key));
        if (body_failed(s_body)) { on_failure(path, 0, 0, "SYNTHESIZER"); return; }
        auto items = depot::parse_funnel_r4(s_body);
        if (items.size() > 100) items.resize(100);
        agent_log(path, "SYNTHESIZER", "DONE items=" + std::to_string(items.size()) +
                  " bodylen=" + std::to_string(s_body.size()));

        std::unordered_set<string> seen;
        seen.insert(depot::canonical(node.title));  // never a child of itself
        for (auto& t : existing) seen.insert(depot::canonical(t));
        vector<depot::FunnelItem> keep;
        for (auto& itm : items) {
            if (!seen.insert(depot::canonical(itm.title)).second) continue;
            keep.push_back(itm);
        }

        bool low = crit.score < RETRY_SCORE;
        int kept = (int)keep.size(), atom = 0;
        vector<std::pair<string, bool>> added;
        {
            std::lock_guard<std::mutex> lk(m);
            std::ofstream f;
            if (!dry) f.open(level_file_for(root, segs), std::ios::app);
            for (auto& itm : keep) {
                int id = tk_next.fetch_add(1);
                char idbuf[16];
                std::snprintf(idbuf, sizeof idbuf, "TK%05d", id);
                string cid = idbuf, cpath = path + "-" + cid;
                if (itm.atomic) atom++;
                if (!dry) f << cid << "," << esc_csv_field(cpath) << "," << esc_csv_field(itm.title) << "\n";
                by_path[cpath] = FunnelNode{"L" + std::to_string(segs), cpath, itm.title};
                children_of[path].push_back(cpath);
                added.push_back({cpath, itm.atomic});
            }
            if (!dry) {
                std::ofstream cr(join_path(root, "funnel_critique.tsv"), std::ios::app);
                cr << path << "\t" << crit.score << "\t" << depot::sanitize_tsv(crit.issues) << "\t"
                   << [&]{ string o; for (auto& t : crit.remove) o += t + ";"; return o; }() << "\t"
                   << [&]{ string o; for (auto& t : crit.add) o += t + ";"; return o; }() << "\n";
                // per-item atomic flags (leaf items that cannot be decomposed)
                bool any_atomic = false;
                for (auto& pr : added)
                    if (pr.second) { any_atomic = true; break; }
                if (any_atomic) {
                    std::ofstream at(join_path(root, "atomic_items.tsv"), std::ios::app);
                    for (auto& pr : added)
                        if (pr.second) at << pr.first << "\t1\n";
                }
            }
            if (!low) {
                int retries = 0;
                auto ri = state_retries.find(path);
                if (ri != state_retries.end()) retries = ri->second;
                append_funnel_state(join_path(root, "funnel_state.tsv"), path, kept, atom,
                                    retries, crit.score, dry);
                done.insert(path);
            }
        }
        kept_total += kept;
        atomic_total += atom;
        {
            std::lock_guard<std::mutex> lk(m);
            std::cout << "NODE_DONE\t" << path << "\t" << kept << "\t" << atom
                      << "\t" << crit.score << (low ? "\tRETRY-QUEUED" : "\tFINAL")
                      << "\tcalls=" << call_total.load() << std::endl;
        }
        for (auto& pr : added) {
            const string& cpath = pr.first;
            if (!pr.second) {
                std::lock_guard<std::mutex> lk(m);
                if (deep)
                    queue.push_front(cpath);   // depth-first: descend the new chain
                else
                    queue.push_back(cpath);
            }
        }
        if (low) {
            // next round can fix what this one missed (max 1 retry per node);
            // --deep skips retries entirely so every loop settles a new node
            std::lock_guard<std::mutex> lk(m);
            if (retry_count[path] < 1 && !deep) {
                retry_issues[path] = crit.issues;
                retry_count[path]++;
                retried++;
                queue.push_back(path);
            } else {
                // budget spent on this node; settle it so later rounds don't re-run it
                int retries = 1;
                auto ri = state_retries.find(path);
                if (ri != state_retries.end()) retries = ri->second + retry_count[path];
                append_funnel_state(join_path(root, "funnel_state.tsv"), path, kept, atom,
                                    retries, crit.score, dry);
                done.insert(path);
            }
        }
    };

    std::cout << "RUN_START\tbudget=" << budget << "\tseeds=" << seeds.size()
              << "\trounds=" << rounds << std::endl;

    // ---- live-resizable worker pool ----
    // A manager (this thread) reconciles the live worker count with the target
    // read from control/concurrency.txt (written by the machine supervisor).
    // Workers retire gracefully between nodes; in-flight nodes always finish.
    struct Worker {
        std::thread t;
        std::atomic<bool> retire{false};
    };
    std::vector<std::unique_ptr<Worker>> workers;
    std::mutex wm;  // guards `workers` + live count
    std::condition_variable cv;
    std::atomic<int> live{0};
    std::atomic<int> active_nodes{0};
    std::atomic<int> in_flight{0};      // started-but-not-finished loops
    std::atomic<bool> shutdown{false};

    auto control_target = [&]() -> int {
        std::ifstream f(join_path(root, "control/concurrency.txt"));
        string line;
        if (std::getline(f, line)) {
            int v = std::atoi(depot::trim(line).c_str());
            if (v >= 1 && v <= 5000) return v;
        }
        return -1;  // no valid control file -> keep current target
    };

    auto worker_fn = [&](Worker* w) {
        while (true) {
            string path;
            {
                std::unique_lock<std::mutex> lk(m);
                auto budget_full = [&] {
                    return budget < 2000000000 &&
                           funneled.load() + in_flight.load() >= budget;
                };
                cv.wait(lk, [&] {
                    return !queue.empty() || shutdown.load() || w->retire.load() ||
                           budget_full();
                });
                if (shutdown.load() || w->retire.load() || budget_full()) return;
                path = queue.front();
                queue.pop_front();
                in_flight++;
                active_nodes++;
                std::cout << "QUEUE\t" << queue.size() << "\t" << funneled.load()
                          << "\t" << budget << std::endl;
            }
            bool skip_done = false;
            {
                std::lock_guard<std::mutex> lk(m);
                skip_done = done.count(path) && !force;
            }
            if (!skip_done) {
                try {
                    work(path);
                } catch (const std::exception& e) {
                    // thread exhaustion (std::async), bad_alloc, or any other
                    // unexpected failure must settle the node, not terminate
                    // the whole process.
                    {
                        std::lock_guard<std::mutex> lk(m);
                        errors++;
                        std::cout << "NODE_FAIL\t" << path << "\texception: "
                                  << e.what() << std::endl;
                        if (retry_count[path] < 1) {
                            retry_count[path]++;
                            queue.push_back(path);
                        } else {
                            int retries = 1;
                            auto ri = state_retries.find(path);
                            if (ri != state_retries.end())
                                retries = ri->second + retry_count[path];
                            append_funnel_state(join_path(root, "funnel_state.tsv"),
                                                path, 0, 0, retries, 0.0, dry);
                            done.insert(path);
                        }
                    }
                }
                funneled++;
            }
            in_flight--;
            active_nodes--;
            cv.notify_all();
        }
    };

    auto spawn_workers = [&](int n) {
        std::lock_guard<std::mutex> lk(wm);
        for (int i = 0; i < n; ++i) {
            auto w = std::make_unique<Worker>();
            w->t = std::thread(worker_fn, w.get());
            workers.push_back(std::move(w));
            live++;
        }
    };
    auto retire_workers = [&](int n) {
        int marked = 0;
        {
            std::lock_guard<std::mutex> lk(wm);
            for (auto it = workers.rbegin(); it != workers.rend() && marked < n; ++it) {
                (*it)->retire.store(true);
                marked++;
            }
        }
        cv.notify_all();
    };
    auto reap_retired = [&] {
        std::lock_guard<std::mutex> lk(wm);
        for (auto it = workers.begin(); it != workers.end();) {
            if ((*it)->retire.load()) {
                if ((*it)->t.joinable()) (*it)->t.join();
                live--;
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
    };

    spawn_workers(std::max(1, concurrency));
    int target = std::max(1, concurrency);
    std::cout << "THREADS\t" << target << "\t" << live.load() << std::endl;  // initial pool size
    int round_idx = 0;
    int ctrl_ms = 3000;
    if (const char* e = std::getenv("DEPOT_CONTROL_INTERVAL_MS"))
        ctrl_ms = std::max(50, std::atoi(e));

    auto drained = [&] {
        std::lock_guard<std::mutex> lk(m);
        return queue.empty() && active_nodes.load() == 0;
    };

    while (!shutdown.load()) {
        // round seeding: when drained and rounds remain and budget not reached
        if (drained() && round_idx < rounds && funneled.load() < budget) {
            {
                std::lock_guard<std::mutex> lk(m);
                for (auto& sd : seeds) {
                    vector<string> stk{sd};
                    while (!stk.empty()) {
                        string p = stk.back(); stk.pop_back();
                        if (done.count(p) && !force) {
                            auto it = children_of.find(p);
                            if (it != children_of.end())
                                for (auto& c : it->second) stk.push_back(c);
                        } else {
                            queue.push_back(p);
                            if (force) {
                                // force re-runs the node AND walks into its
                                // children so the whole subtree is re-queued
                                auto it = children_of.find(p);
                                if (it != children_of.end())
                                    for (auto& c : it->second) stk.push_back(c);
                            }
                        }
                    }
                }
            }
            round_idx++;
            std::cout << "ROUND_START\t" << round_idx << "\tof " << rounds << std::endl;
            cv.notify_all();
        }
        // completion: budget reached (stop immediately) or all rounds drained
        if ((funneled.load() >= budget && active_nodes.load() == 0) ||
            (round_idx >= rounds && drained())) {
            shutdown.store(true);
            cv.notify_all();
            break;
        }
        // wait (interruptible), then reconcile with the machine supervisor
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::milliseconds(ctrl_ms));
        }
        int want = control_target();
        if (want > 0 && want != target) {
            if (want > target) spawn_workers(want - target);
            else retire_workers(target - want);
            target = want;
            std::cout << "THREADS\t" << target << "\t" << live.load() << std::endl;
        }
        reap_retired();
    }
    reap_retired();
    {
        std::lock_guard<std::mutex> lk(wm);
        for (auto& w : workers)
            if (w->t.joinable()) w->t.join();
        workers.clear();
        live.store(0);
    }
    std::cout << "funneled=" << funneled.load() << " kept=" << kept_total.load()
              << " atomic=" << atomic_total.load() << " calls=" << call_total.load()
              << " retried=" << retried.load() << " errors=" << errors.load() << "\n";
    return 0;
}

// ---------------- llm command ----------------
static string curl_post(const string& payload, const string& key) {
    // mkstemp: unique names + 0600 from creation — no predictable path, no
    // symlink planting, no chmod-after-write window for the API key.
    char tmp[] = "/tmp/depot_llm_XXXXXX.json";
    char hdr[] = "/tmp/depot_llm_XXXXXX.hdr";
    int tfd = mkstemp(tmp);
    if (tfd < 0) return "";
    int hfd = mkstemp(hdr);
    if (hfd < 0) { close(tfd); unlink(tmp); return ""; }
    {
        std::string hp = "Content-Type: application/json\nAuthorization: Bearer " + key + "\n";
        const char* p = payload.data(); size_t n = payload.size();
        const char* hp2 = hp.data(); size_t hn = hp.size();
        while (n) { ssize_t w = write(tfd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
        while (hn) { ssize_t w = write(hfd, hp2, hn); if (w <= 0) break; hp2 += w; hn -= (size_t)w; }
    }
    close(tfd);
    close(hfd);
    string cmd = "curl -4 -s --max-time 120 " + api_url_for(key) + " -H @" + hdr + " --data-binary @" + tmp;
    FILE* p = popen(cmd.c_str(), "r");
    string out;
    int rc = -1;
    if (p) {
        char buf[4096];
        while (fgets(buf, sizeof buf, p)) out += buf;
        rc = pclose(p);
    }
    std::remove(tmp);
    std::remove(hdr);
    if (rc != 0) return "";  // HTTP/transport failure must not look like a success
    return out;
}

static depot::LlmAnswer call_llm(const string& layer, const string& path, const string& title,
                                 const string& parent, int children, const string& key,
                                 const string& model) {
    string user = "Layer: " + layer + "\nPath: " + path + "\nTitle: " + title +
                  "\nParent: " + parent + "\nChildren: " + std::to_string(children) + "\n"
                  "Return JSON: {\"valid\": true or false, \"fixed\": null or corrected title string, "
                  "\"admits_children\": true or false}. A valid title is Title Case, concise, no question "
                  "marks, no emoji, no trailing punctuation. admits_children=true only if the item "
                  "naturally decomposes into 3-10 sub-items. For layers L0-L4 always true.";
    string sys = "You are a strict data-quality checker for a hierarchical work-breakdown "
                 "knowledge base that powers AI agents. Answer ONLY with a JSON object, no other text.";
    string payload = "{\"model\":\"" + model + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" +
                     depot::json_escape(sys) + "\"},{\"role\":\"user\",\"content\":\"" +
                     depot::json_escape(user) + "\"}],\"max_tokens\":400,\"temperature\":0,"
                     "\"stream\":false," + reasoning_param(key) + "}";
    return depot::parse_llm(curl_post(payload, key));
}

struct Node {
    string layer, path, title, parent;
    int children = 0;
    string file;
};

int depot::run_llm(const vector<int>& levels, int limit, int offset, int concurrency,
                   bool dry, bool force, const string& model, const string& key,
                   const string& root) {
    const vector<string> lv = {"L0","L1","L2","L3","L4","L5","L6","L7","L8","L9"};
    vector<string> files = {"Onion - level_00.csv","Onion - level_01.csv","Onion - level_02.csv",
                            "Onion - level_03.csv","Onion - level_04.csv","Onion - level_05.csv",
                            "Onion - level_06.csv","Onion - level_07.csv","Onion - level_08.csv",
                            "Onion - level_09.csv"};
    std::unordered_map<string, Node> by_path;
    vector<Node> ordered;
    for (size_t i = 0; i < lv.size(); ++i) {
        std::ifstream f(join_path(root, files[i]));
        string line; std::getline(f, line);
        while (std::getline(f, line)) {
            if (depot::trim(line).empty()) continue;
            auto c = depot::split_csv(line);
            if (c.size() < 2) continue;
            Node n;
            n.layer = lv[i]; n.file = files[i];
            if (i == 0) { n.path = depot::trim(c[0]); n.title = depot::trim(c[1]); }
            else { if (c.size() < 3) continue; n.path = depot::trim(c[1]); n.title = depot::trim(c[2]); }
            if (!n.path.empty()) by_path[n.path] = n;
        }
    }
    // one-pass parent -> children index (O(N))
    std::unordered_map<string, vector<string>> children_of;
    for (auto& kv : by_path) {
        size_t d = kv.first.rfind('-');
        if (d != string::npos) children_of[kv.first.substr(0, d)].push_back(kv.first);
    }
    // DFS from L0 roots (sorted for deterministic order)
    vector<string> root_keys;
    for (auto& kv : by_path) if (kv.first.find('-') == string::npos) root_keys.push_back(kv.first);
    std::sort(root_keys.begin(), root_keys.end());
    vector<Node> stack;
    for (auto& k : root_keys) stack.push_back(by_path[k]);
    std::unordered_set<string> seen;
    while (!stack.empty()) {
        Node n = stack.back(); stack.pop_back();
        if (!seen.insert(n.path).second) continue;
        size_t d = n.path.rfind('-');
        if (d != string::npos) {
            auto it = by_path.find(n.path.substr(0, d));
            if (it != by_path.end()) n.parent = it->second.title;
        }
        n.children = (int)children_of[n.path].size();
        ordered.push_back(n);
        auto& kids = children_of[n.path];
        std::sort(kids.begin(), kids.end(), std::greater<string>());
        for (auto& k : kids) stack.push_back(by_path[k]);
    }

    std::unordered_set<string> done;
    {
        std::ifstream f(join_path(root, "node_llm_report.tsv"));
        string line;
        while (std::getline(f, line)) {
            size_t t = line.find('\t');
            if (t != string::npos) done.insert(line.substr(0, t));
        }
    }
    std::unordered_set<int> want(levels.begin(), levels.end());
    vector<const Node*> todo;
    for (auto& n : ordered)
        if (want.count(std::atoi(n.layer.c_str() + 1)) && (!done.count(n.path) || force))
            todo.push_back(&n);
    if (offset > 0) todo.erase(todo.begin(), todo.begin() + std::min<size_t>(offset, todo.size()));
    if (limit > 0 && todo.size() > (size_t)limit) todo.resize(limit);
    std::cout << "nodes to check: " << todo.size() << " | model " << model
              << " | workers " << concurrency << "\n";
    std::cout << "traversal: root -> L0 -> L1 -> L2 -> L3 -> L4 -> L5 -> L6 -> L7 -> L8 -> L9\n";

    std::mutex m;
    vector<string> report_lines, fixes, pending;
    std::atomic<int> done_ct{0}, errors{0};
    auto work = [&](const Node* n) {
        try {
            auto a = call_llm(n->layer, n->path, n->title, n->parent, n->children, key, model);
            std::lock_guard<std::mutex> lk(m);
            if (!a.parse_ok) errors++;
            string line = n->path + "\t" + n->layer + "\t" + depot::sanitize_tsv(n->title) + "\t" + depot::sanitize_tsv(n->parent) + "\t" +
                          std::to_string(n->children) + "\t" + (a.valid ? "true" : "false") + "\t" +
                          (a.fixed.empty() ? string("") : a.fixed) + "\t" + (a.admits ? "true" : "false") + "\t" + depot::sanitize_tsv(a.error);
            report_lines.push_back(line);
            if (!a.fixed.empty() && a.fixed != n->title)
                fixes.push_back(n->file + "\t" + n->path + "\t" + n->title + "\t" + a.fixed);
            if (a.parse_ok && a.admits && n->children == 0 &&
                n->layer != "L0" && n->layer != "L1" && n->layer != "L2" &&
                n->layer != "L3" && n->layer != "L4")
                pending.push_back(n->path);
            done_ct++;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(m);
            done_ct++;
            errors++;
            // 9 fields, same layout as the header
            report_lines.push_back(n->path + "\t" + n->layer + "\t" + n->title +
                                   "\t\t\t\t\t" + depot::sanitize_tsv(string(e.what()).substr(0, 160)));
        }
    };
    vector<std::thread> pool;
    std::atomic<size_t> cursor{0};
    for (int i = 0; i < concurrency; ++i)
        pool.emplace_back([&] {
            while (true) {
                size_t idx = cursor.fetch_add(1);
                if (idx >= todo.size()) break;
                work(todo[idx]);
            }
        });
    for (auto& t : pool) t.join();
    std::cout << "done " << done_ct.load() << " | errors " << errors.load()
              << " | fixes " << fixes.size() << " | pending " << pending.size() << "\n";
    if (!dry) {
        bool report_exists = false;
        {
            std::ifstream chk(join_path(root, "node_llm_report.tsv"));
            report_exists = (bool)chk;
        }
        std::ofstream f(join_path(root, "node_llm_report.tsv"), std::ios::app);
        if (!report_exists)
            f << "path\tlayer\ttitle\tparent\tchildren\tvalid\tfixed\tadmits_children\terror\n";
        for (auto& l : report_lines) f << l << "\n";
        if (!fixes.empty()) {
            std::unordered_map<string, std::unordered_map<string, string>> renames;
            for (auto& fx : fixes) {
                std::stringstream ss(fx);
                string file, path, old, newt;
                std::getline(ss, file, '\t'); std::getline(ss, path, '\t');
                std::getline(ss, old, '\t'); std::getline(ss, newt, '\t');
                renames[file][path] = newt;
            }
            for (auto& kv : renames) {
                auto rows = load_rows(join_path(root, kv.first));
                int changed = 0;
                for (auto& r : rows) if (kv.second.count(r.path) && r.title != kv.second.at(r.path)) {
                    r.title = kv.second.at(r.path); changed++;
                }
                size_t lp = kv.first.find("level_");
                int lnum = (lp != string::npos) ? std::atoi(kv.first.c_str() + lp + 6) : 0;
                vector<string> hdr = {"ID", "ID-L" + std::to_string(lnum), "Title"};
                if (kv.first == "Onion - level_00.csv") hdr = {"ID", "Phase"};
                if (kv.first == "Onion - level_01.csv") hdr = {"ID", "ID-L1", "DOC"};
                if (kv.first == "Onion - level_02.csv") hdr = {"ID", "ID-L2", "Tasks & Activities"};
                write_rows(join_path(root, kv.first), hdr, rows);
                std::cout << "applied " << changed << " title fixes to " << kv.first << "\n";
            }
        }
        if (!pending.empty()) {
            std::unordered_set<string> already;
            {
                std::ifstream f0(join_path(root, "llm_pending_expansion.txt"));
                string l0;
                while (std::getline(f0, l0))
                    if (!depot::trim(l0).empty()) already.insert(depot::trim(l0));
            }
            std::ofstream f(join_path(root, "llm_pending_expansion.txt"), std::ios::app);
            for (auto& p : pending)
                if (!already.count(p)) f << p << "\n";
        }
    }
    return 0;
}
