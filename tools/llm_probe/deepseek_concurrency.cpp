// deepseek_concurrency_test.cpp
//
// Finds how many parallel requests make DeepSeek fastest for YOUR account.
// It runs the same small request at several concurrency levels (one thread =
// one request in flight) and reports, per level: answers per second, latency,
// and errors (429 rate limits, 5xx, timeouts). The best level is the smallest
// one that reaches ~90% of the top speed with no errors.
//
// Build:
//   Linux (Kali/Ubuntu): sudo apt install libcurl4-openssl-dev
//                        g++ -O2 -std=c++17 deepseek_concurrency_test.cpp -lcurl -pthread -o ds_test
//   macOS:               clang++ -O2 -std=c++17 deepseek_concurrency_test.cpp -lcurl -o ds_test
//
// Run:
//   export DEEPSEEK_API_KEY=sk-...           (read from the environment, never printed)
//   ./ds_test --model <id from GET https://api.deepseek.com/models>
//   ./ds_test --model <id> --levels 12,1000  (just the head-to-head)
//
// Options:
//   --levels 1,4,12,25,50,100,200   concurrency levels to test, in order
//   --per-level N     requests per level (default: max(30, 3 x level))
//   --per-thread N    requests per thread at every level: N x level (minimum 2 for a steady rate)
//   --ramp S          spread thread starts over S seconds (default 0), to avoid a burst of new connections
//   --stop-flat P     stop when a level is less than P% faster than the previous one (limit reached)
//   --max-tokens N    answer length per request (default 16: cheap)
//   --model ID        model id (default deepseek-chat; check /models)
//   --url URL         endpoint (default https://api.deepseek.com/chat/completions)
//   --timeout S       per-request timeout in seconds (default 120)
//   --reasoning-none  add "reasoning_effort":"none" to each request (recommended: Onion sends it)
//   --list-items N    ask for a numbered list of N items instead of one word, so answers
//                     are long like real Onion replies (max_tokens defaults to N x 15 + 50)
//   --max-total N     safety cap on total requests (default 5000)
//   --price-in X --price-out Y   USD per 1M tokens, to print a cost estimate
//   --csv FILE        also write the results table as CSV
//   --no-stop         keep going after a level shows errors
//   --yes             skip the confirmation prompt

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

using Clock = std::chrono::steady_clock;

struct Options {
    std::vector<int> levels{1, 4, 12, 25, 50, 100, 200};
    int per_level = 0;  // 0 = auto
    int per_thread = 0; // 0 = auto
    double ramp_s = 0;
    double stop_flat_pct = 0;
    int max_tokens = 16;
    std::string model = "deepseek-chat";
    std::string url = "https://api.deepseek.com/chat/completions";
    long timeout_s = 120;
    bool reasoning_none = false;
    int list_items = 0;
    long max_total = 5000;
    double price_in = 0, price_out = 0;
    std::string csv;
    bool stop_on_errors = true;
    bool yes = false;
};

enum class Kind { Ok, RateLimited, Server, Timeout, Network, AuthOrBalance, Other };

struct Result {
    Kind kind = Kind::Other;
    double latency_s = 0;
    long http = 0;
    long prompt_tokens = 0, completion_tokens = 0;
    bool truncated = false;
    int curl_code = 0;        // why a transport failure happened
    bool connected = false;   // false = never reached the server
};

struct LevelStats {
    int level = 0, started_threads = 0, requests = 0;
    int ok = 0, rate_limited = 0, server = 0, timeout = 0, network = 0, other = 0, truncated = 0;
    bool burst = false;  // fewer than 2 requests per thread: a single burst, not a steady rate
    double wall_s = 0, per_sec = 0, tokens_per_sec = 0, p50 = 0, p95 = 0, p99 = 0;
    long tokens_in = 0, tokens_out = 0;
    int timeout_connecting = 0, timeout_answering = 0;
    std::map<int, int> net_causes;  // curl error code -> count
    double error_rate() const { return requests ? double(rate_limited + server + timeout + network + other) / requests : 0; }
};

// One shared DNS cache and TLS session cache for all threads: the address is looked up once
// instead of once per thread, which home routers cannot handle at hundreds at a time.
static CURLSH* g_share = nullptr;
static std::mutex g_share_mu[CURL_LOCK_DATA_LAST];
static void share_lock(CURL*, curl_lock_data d, curl_lock_access, void*) { g_share_mu[d].lock(); }
static void share_unlock(CURL*, curl_lock_data d, void*) { g_share_mu[d].unlock(); }

static std::atomic<bool> g_abort{false};
static std::string g_abort_reason;
static std::mutex g_abort_mu;

static size_t write_cb(char* p, size_t s, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append(p, s * n);
    return s * n;
}

// Reads an integer field like "completion_tokens":12 from the reply.
static long json_int(const std::string& body, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return 0;
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return 0;
    return std::strtol(body.c_str() + p + 1, nullptr, 10);
}

static std::vector<int> parse_levels(const std::string& s) {
    std::vector<int> v;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (int n = std::atoi(item.c_str()); n > 0) v.push_back(n);
    return v;
}

static bool safe_model(const std::string& m) {
    if (m.empty()) return false;
    for (char c : m)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
    return true;
}

static int requests_for(const Options& o, int level) {
    if (o.per_level > 0) return o.per_level;
    if (o.per_thread > 0) return std::max(30, o.per_thread * level);
    return std::max(30, 3 * level);
}

// Lets 1,000 connections exist at once: macOS defaults to only 256 open files.
static void raise_open_file_limit() {
#if !defined(_WIN32)
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t want = rl.rlim_max;
    if (want == RLIM_INFINITY || want > 10240) want = 10240;  // macOS refuses "unlimited"
    if (rl.rlim_cur < want) {
        rl.rlim_cur = want;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    std::cout << "open-file limit: " << rl.rlim_cur << "\n";
#endif
}

static void worker(const Options& o, const std::string& auth_header, std::atomic<int>& next, int total,
                   std::vector<Result>& results) {
    CURL* h = curl_easy_init();  // one handle per thread: its connection is reused
    if (!h) return;
    curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth_header.c_str());
    curl_easy_setopt(h, CURLOPT_URL, o.url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, o.timeout_s);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (g_share) curl_easy_setopt(h, CURLOPT_SHARE, g_share);
    curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);

    std::string body, reply;
    for (;;) {
        if (g_abort.load()) break;
        int i = next.fetch_add(1);
        if (i >= total) break;
        // A different id per request keeps answers from being served from DeepSeek's prompt cache.
        const std::string ask = o.list_items > 0
            ? "List " + std::to_string(o.list_items) + " distinct sub-tasks of planning a software project, "
              "one short line each, numbered. Request id " + std::to_string(i) + "."
            : "Reply with the single word OK. Request id " + std::to_string(i) + ".";
        body = "{\"model\":\"" + o.model + "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + ask + "\"}],"
               "\"max_tokens\":" + std::to_string(o.max_tokens) + ",\"temperature\":0,\"stream\":false" +
               (o.reasoning_none ? ",\"reasoning_effort\":\"none\"" : "") + "}";
        reply.clear();
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &reply);

        auto t0 = Clock::now();
        CURLcode rc = curl_easy_perform(h);
        double dt = std::chrono::duration<double>(Clock::now() - t0).count();

        Result r;
        r.latency_s = dt;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.http);
        double connect_s = 0;
        curl_easy_getinfo(h, CURLINFO_CONNECT_TIME, &connect_s);
        r.connected = connect_s > 0;
        r.curl_code = static_cast<int>(rc);
        if (rc == CURLE_OPERATION_TIMEDOUT) {
            r.kind = Kind::Timeout;
        } else if (rc != CURLE_OK) {
            r.kind = Kind::Network;
        } else if (r.http == 200 && reply.find("\"choices\"") != std::string::npos) {
            r.kind = Kind::Ok;
            r.prompt_tokens = json_int(reply, "prompt_tokens");
            r.completion_tokens = json_int(reply, "completion_tokens");
            r.truncated = reply.find("\"finish_reason\":\"length\"") != std::string::npos;
        } else if (r.http == 429) {
            r.kind = Kind::RateLimited;
        } else if (r.http >= 500) {
            r.kind = Kind::Server;
        } else if (r.http == 401 || r.http == 402) {
            r.kind = Kind::AuthOrBalance;
            std::lock_guard<std::mutex> lk(g_abort_mu);
            if (!g_abort.exchange(true))
                g_abort_reason = r.http == 401 ? "HTTP 401: the API key was rejected"
                                               : "HTTP 402: insufficient balance on the account";
        } else {
            r.kind = Kind::Other;
            static std::once_flag shown;
            std::call_once(shown, [&] {  // show one example so a bad model id is easy to spot
                std::cerr << "  example unexpected reply (HTTP " << r.http << "): "
                          << reply.substr(0, 300) << "\n";
            });
        }
        results[i] = r;
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
}

static double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * (v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
}

static LevelStats run_level(const Options& o, const std::string& auth_header, int level) {
    LevelStats st;
    st.level = level;
    st.requests = requests_for(o, level);
    st.burst = st.requests < 2 * level;
    std::vector<Result> results(st.requests);
    std::atomic<int> next{0};
    std::vector<std::thread> threads;
    threads.reserve(level);

    auto t0 = Clock::now();
    for (int t = 0; t < level; ++t) {
        if (o.ramp_s > 0 && t > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(o.ramp_s / level));
        try {
            threads.emplace_back(worker, std::cref(o), std::cref(auth_header), std::ref(next), st.requests,
                                 std::ref(results));
        } catch (const std::exception& e) {
            std::cerr << "  could only start " << t << " threads: " << e.what() << "\n";
            break;
        }
    }
    st.started_threads = static_cast<int>(threads.size());
    for (auto& th : threads) th.join();
    st.wall_s = std::chrono::duration<double>(Clock::now() - t0).count();

    std::vector<double> lat;
    int done = std::min(next.load(), st.requests);
    st.requests = done;
    for (int i = 0; i < done; ++i) {
        const Result& r = results[i];
        switch (r.kind) {
            case Kind::Ok:
                st.ok++;
                lat.push_back(r.latency_s);
                st.tokens_in += r.prompt_tokens;
                st.tokens_out += r.completion_tokens;
                if (r.truncated) st.truncated++;
                break;
            case Kind::RateLimited: st.rate_limited++; break;
            case Kind::Server: st.server++; break;
            case Kind::Timeout:
                st.timeout++;
                (r.connected ? st.timeout_answering : st.timeout_connecting)++;
                break;
            case Kind::Network:
                st.network++;
                st.net_causes[r.curl_code]++;
                break;
            default: st.other++; break;
        }
    }
    st.per_sec = st.wall_s > 0 ? st.ok / st.wall_s : 0;
    st.tokens_per_sec = st.wall_s > 0 ? st.tokens_out / st.wall_s : 0;
    st.p50 = percentile(lat, 0.50);
    st.p95 = percentile(lat, 0.95);
    st.p99 = percentile(lat, 0.99);
    return st;
}

static void print_row(const LevelStats& s) {
    std::printf("%7d %8d %6d %6d %5d %5d %5d %5d  %8.2f %9.1f  %6.2f %6.2f %6.2f\n", s.level, s.requests, s.ok,
                s.rate_limited, s.server, s.timeout, s.network, s.other, s.per_sec, s.tokens_per_sec, s.p50, s.p95,
                s.p99);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { std::cerr << name << " needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--levels") o.levels = parse_levels(val("--levels"));
        else if (a == "--per-level") o.per_level = std::atoi(val("--per-level").c_str());
        else if (a == "--per-thread") o.per_thread = std::atoi(val("--per-thread").c_str());
        else if (a == "--ramp") o.ramp_s = std::atof(val("--ramp").c_str());
        else if (a == "--stop-flat") o.stop_flat_pct = std::atof(val("--stop-flat").c_str());
        else if (a == "--max-tokens") o.max_tokens = std::atoi(val("--max-tokens").c_str());
        else if (a == "--model") o.model = val("--model");
        else if (a == "--url") o.url = val("--url");
        else if (a == "--timeout") o.timeout_s = std::atol(val("--timeout").c_str());
        else if (a == "--reasoning-none") o.reasoning_none = true;
        else if (a == "--list-items") o.list_items = std::atoi(val("--list-items").c_str());
        else if (a == "--max-total") o.max_total = std::atol(val("--max-total").c_str());
        else if (a == "--price-in") o.price_in = std::atof(val("--price-in").c_str());
        else if (a == "--price-out") o.price_out = std::atof(val("--price-out").c_str());
        else if (a == "--csv") o.csv = val("--csv");
        else if (a == "--no-stop") o.stop_on_errors = false;
        else if (a == "--yes") o.yes = true;
        else { std::cerr << "unknown option " << a << " (see the comment at the top of the file)\n"; return 2; }
    }
    bool max_tokens_given = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--max-tokens") max_tokens_given = true;
    if (o.list_items > 0 && !max_tokens_given) o.max_tokens = o.list_items * 15 + 50;
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (!key || !*key) { std::cerr << "set DEEPSEEK_API_KEY first\n"; return 2; }
    if (!safe_model(o.model)) { std::cerr << "model id has unexpected characters\n"; return 2; }
    if (o.levels.empty() || o.max_tokens < 1 || o.timeout_s < 1) { std::cerr << "bad options\n"; return 2; }

    long total = 0;
    for (int l : o.levels) total += requests_for(o, l);
    if (total > o.max_total) {
        std::cerr << "plan needs " << total << " requests, above --max-total " << o.max_total
                  << ". Lower --per-level or the levels, or raise --max-total.\n";
        return 2;
    }

    // Rough worst case: ~30 input tokens per request, max_tokens output each.
    long est_in = total * 30, est_out = total * static_cast<long>(o.max_tokens);
    std::cout << "Plan: levels";
    for (int l : o.levels) std::cout << " " << l;
    std::cout << " | " << total << " requests | model " << o.model << " | max_tokens " << o.max_tokens << "\n"
              << "Worst case about " << est_in << " input + " << est_out << " output tokens";
    if (o.price_in > 0 || o.price_out > 0)
        std::printf(" = about $%.4f", est_in / 1e6 * o.price_in + est_out / 1e6 * o.price_out);
    std::cout << "\n";
    for (int l : o.levels)
        if (requests_for(o, l) < 2 * l)
            std::cout << "Warning: level " << l << " gets only " << requests_for(o, l)
                      << " requests (< 2 per thread). It will measure one burst, not a steady rate, "
                         "and is left out of the recommendation. Raise --per-level to at least "
                      << 2 * l << ".\n";
    if (!o.yes) {
        std::cout << "Type yes to start: " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "yes") { std::cout << "cancelled\n"; return 0; }
    }

    raise_open_file_limit();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
    const std::string auth_header = std::string("Authorization: Bearer ") + key;

    std::printf("\n  level requests     ok    429   5xx  tout   net other   answers/s  tokens/s  p50(s) p95(s) p99(s)\n");
    std::vector<LevelStats> all;
    for (int level : o.levels) {
        LevelStats s = run_level(o, auth_header, level);
        all.push_back(s);
        print_row(s);
        if (s.timeout)
            std::printf("        timeouts: %d while connecting, %d waiting for the answer\n",
                        s.timeout_connecting, s.timeout_answering);
        for (const auto& [code, n] : s.net_causes)
            std::printf("        net errors: %d x %s\n", n, curl_easy_strerror(static_cast<CURLcode>(code)));
        if (s.burst)
            std::printf("        note: burst only (fewer than 2 requests per thread); not used for the recommendation\n");
        if (s.started_threads < level)
            std::printf("        note: only %d of %d threads started\n", s.started_threads, level);
        if (s.truncated)
            std::printf("        note: %d replies hit max_tokens; consider --reasoning-none or a larger --max-tokens\n",
                        s.truncated);
        if (g_abort.load()) { std::cout << "\nStopped: " << g_abort_reason << "\n"; break; }
        if (o.stop_flat_pct > 0 && !s.burst && s.error_rate() < 0.05) {
            const LevelStats* prev = nullptr;
            for (size_t k = all.size() - 1; k-- > 0;)
                if (!all[k].burst && all[k].error_rate() < 0.05) { prev = &all[k]; break; }
            if (prev && s.per_sec < prev->per_sec * (1.0 + o.stop_flat_pct / 100.0)) {
                std::printf("\nStopped: speed stopped growing (%d -> %d in flight gave %.2f -> %.2f answers/s, "
                            "under +%.0f%%).\nThe limit is around %d-%d parallel requests.\n",
                            prev->level, s.level, prev->per_sec, s.per_sec, o.stop_flat_pct, prev->level, s.level);
                break;
            }
        }
        if (o.stop_on_errors && s.error_rate() >= 0.05) {
            int conn = s.network + s.timeout_connecting;
            if (conn * 2 > s.rate_limited + s.server + s.timeout + s.network + s.other)
                std::printf("\nStopped: %.0f%% errors at %d parallel requests, mostly connection failures.\n"
                            "That points to the network or the provider blocking connections, not slow answers.\n"
                            "Check your connection and the provider's status page, wait, then retry a low level.\n",
                            s.error_rate() * 100, level);
            else
                std::printf("\nStopped: %.0f%% errors at %d parallel requests: the provider is saturated here.\n",
                            s.error_rate() * 100, level);
            break;
        }
    }
    if (g_share) curl_share_cleanup(g_share);
    curl_global_cleanup();

    if (!o.csv.empty()) {
        std::ofstream f(o.csv);
        f << "level,requests,ok,rate_limited,server,timeout,network,other,answers_per_s,tokens_per_s,p50_s,p95_s,p99_s\n";
        for (const auto& s : all)
            f << s.level << ',' << s.requests << ',' << s.ok << ',' << s.rate_limited << ',' << s.server << ','
              << s.timeout << ',' << s.network << ',' << s.other << ',' << s.per_sec << ',' << s.tokens_per_sec
              << ',' << s.p50 << ',' << s.p95 << ',' << s.p99 << '\n';
        std::cout << "CSV written to " << o.csv << "\n";
    }

    // Best = the smallest level that reaches 90% of the top speed with under 1% errors.
    std::vector<LevelStats> steady;
    for (const auto& s : all)
        if (!s.burst) steady.push_back(s);
    if (steady.size() == 1 && steady[0].error_rate() < 0.01) {
        std::printf("\nOnly one level tested, so there is nothing to compare: %d parallel requests ran cleanly "
                    "(%.2f answers/s).\nTest several levels to find the best one.\n",
                    steady[0].level, steady[0].per_sec);
        return 0;
    }
    double best_speed = 0;
    for (const auto& s : steady)
        if (s.error_rate() < 0.01) best_speed = std::max(best_speed, s.per_sec);
    if (best_speed <= 0) {
        std::cout << "\nNo steady, error-free level, so no recommendation. Check the notes above.\n";
        return 1;
    }
    // If speed was still clearly rising at the last clean level, the limit was not reached.
    if (steady.size() >= 2) {
        const LevelStats& last = steady.back();
        const LevelStats& prev = steady[steady.size() - 2];
        if (last.error_rate() < 0.01 && last.per_sec >= 1.1 * prev.per_sec) {
            std::printf("\nNo limit found yet: speed was still growing at %d parallel requests "
                        "(%.2f -> %.2f answers/s).\nTest higher levels, e.g. --levels %d,%d,%d.\n",
                        last.level, prev.per_sec, last.per_sec, last.level, last.level * 2, last.level * 4);
            return 0;
        }
    }
    for (const auto& s : steady) {
        if (s.error_rate() < 0.01 && s.per_sec >= 0.9 * best_speed) {
            std::printf("\nRecommended: about %d parallel requests (%.2f answers/s, p50 %.2f s).\n", s.level,
                        s.per_sec, s.p50);
            std::printf("Above that, speed grows by less than 10%% or errors appear.\n");
            break;
        }
    }
    return 0;
}
