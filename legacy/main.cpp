// main.cpp — CLI for the Onion data-depot.
//   ingest: lexical-trust ingestion loop
//   llm   : stateless per-node DeepSeek loop over L0..L9 (curl, no memory)
//
// Usage:
//   data_depot ingest research.tsv [--dry] [--restrict P01-IN01-] [--root .]
//   data_depot llm [--levels 0,1,2,3,4,5,6,7,8,9] [--limit N] [--offset N]
//                  [--concurrency N] [--dry] [--force] [--root .]
//   API key: env OPENROUTER_API_KEY (legacy: DEEPSEEK_API_KEY) or edit API_KEY_PLACEHOLDER below.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "depot_core.hpp"
#include "depot_io.hpp"
#ifdef DEPOT_HAVE_OPENCL
#include "depot_gpu.hpp"
#endif

static const char* API_KEY_PLACEHOLDER = "PASTE-YOUR-OPENROUTER-API-KEY-HERE";  // prefer OPENROUTER_API_KEY env

// Key precedence: OPENROUTER_API_KEY > legacy DEEPSEEK_API_KEY > placeholder.
static const char* resolve_key() {
    if (const char* k = std::getenv("OPENROUTER_API_KEY"); k && *k) return k;
    if (const char* k = std::getenv("DEEPSEEK_API_KEY"); k && *k) return k;
    return API_KEY_PLACEHOLDER;
}

// Default model follows the provider the installed key routes to
// (see depot_io.cpp api_url_for): OpenRouter -> Gemini flash-lite, else DeepSeek.
static std::string default_model() {
    return std::string(resolve_key()).rfind("sk-or", 0) == 0
               ? "google/gemini-2.5-flash-lite"
               : "deepseek-flash";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: data_depot <ingest|llm|funnel|gpu> [options]\n";
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "ingest") {
        std::string input, restrict_prefix, root = ".";
        bool dry = false;
        bool use_gpu = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--dry") dry = true;
            else if (a == "--gpu") use_gpu = true;
            else if (a == "--restrict" && i + 1 < argc) restrict_prefix = argv[++i];
            else if (a == "--root" && i + 1 < argc) root = argv[++i];
            else if (input.empty()) input = a;
        }
        if (input.empty()) { std::cerr << "usage: data_depot ingest research.tsv [--dry] [--gpu]\n"; return 2; }
        return depot::run_ingest(input, dry, restrict_prefix, root, use_gpu);
    }
    if (cmd == "gpu") {
#ifdef DEPOT_HAVE_OPENCL
        bool json = false, bench = false;
        int bench_n = 4096, bench_k = 64;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--json") json = true;
            else if (a == "--selftest") {
                std::string err;
                if (depot::gpu::selftest(err)) { std::cout << "gpu selftest OK\n"; return 0; }
                std::cerr << "gpu selftest FAIL: " << err << "\n";
                return 1;
            } else if (a == "--bench") { bench = true; }
            else if (a == "--n" && i + 1 < argc) bench_n = std::atoi(argv[++i]);
            else if (a == "--k" && i + 1 < argc) bench_k = std::atoi(argv[++i]);
        }
        if (bench) {
            static const char* W[] = {"alpha","beta","gamma","delta","epsilon","zeta",
                "eta","theta","kappa","lambda","mu","nu","project","charter","risk",
                "plan","scope","review","data","model","system","process","budget",
                "stakeholder","policy","audit","initiation","governance"};
            const size_t NW = sizeof(W) / sizeof(W[0]);
            std::mt19937 rng(1234);
            auto gen = [&](int n) {
                std::vector<std::string> v;
                v.reserve(n);
                for (int i = 0; i < n; ++i) {
                    int words = 2 + (int)(rng() % 7);
                    std::string t;
                    for (int w = 0; w < words; ++w) {
                        if (w) t += ' ';
                        t += W[rng() % NW];
                    }
                    v.push_back(depot::canonical(t));
                }
                return v;
            };
            auto keys = gen(bench_k), cands = gen(bench_n);
            const double pairs = (double)keys.size() * (double)cands.size();
            auto t0 = std::chrono::steady_clock::now();
            double sink = 0;
            for (const auto& k : keys)
                for (const auto& c : cands) sink += depot::sim_keys(k, c);
            auto t1 = std::chrono::steady_clock::now();
            depot::gpu::SimIndex idx;
            std::string err;
            if (!idx.build(cands, err)) {
                std::cerr << "bench: gpu unavailable: " << err << "\n";
                return 1;
            }
            std::vector<double> sc;
            auto t2 = std::chrono::steady_clock::now();
            if (!idx.scores(keys, sc, err)) { std::cerr << "bench: " << err << "\n"; return 1; }
            auto t3 = std::chrono::steady_clock::now();
            double cpu_s = std::chrono::duration<double>(t1 - t0).count();
            double gpu_s = std::chrono::duration<double>(t3 - t2).count();
            (void)sink;
            std::printf("similarity bench: %d keys x %d cands = %.0f pairs\n",
                        bench_k, bench_n, pairs);
            std::printf("  CPU  %.3fs  (%.0f pairs/s)\n", cpu_s, pairs / (cpu_s > 0 ? cpu_s : 1e-9));
            std::printf("  GPU  %.3fs  (%.0f pairs/s)   [build+upload %.3fs]\n",
                        gpu_s, pairs / (gpu_s > 0 ? gpu_s : 1e-9),
                        std::chrono::duration<double>(t2 - t1).count());
            std::printf("  speedup x%.2f  (on: %s)\n",
                        cpu_s / (gpu_s > 0 ? gpu_s : 1e-9),
                        depot::gpu::sim_device_name().c_str());
            return 0;
        }
        if (!depot::gpu::available()) {
            std::cout << (json ? "{\"opencl\":false,\"devices\":[]}\n"
                               : "no OpenCL platform found (install a runtime, e.g. intel-opencl-icd)\n");
            return 1;
        }
        auto devs = depot::gpu::list_devices();
        if (json) {
            std::cout << "{\"opencl\":true,\"devices\":[";
            for (size_t i = 0; i < devs.size(); ++i) {
                const auto& d = devs[i];
                if (i) std::cout << ",";
                std::cout << "{\"name\":\"" << depot::json_escape(d.name)
                          << "\",\"vendor\":\"" << depot::json_escape(d.vendor)
                          << "\",\"type\":\"" << d.type
                          << "\",\"version\":\"" << depot::json_escape(d.version)
                          << "\",\"driver\":\"" << depot::json_escape(d.driver)
                          << "\",\"global_mem_mb\":" << (d.global_mem_bytes / 1048576ULL)
                          << ",\"max_work_group\":" << d.max_work_group
                          << ",\"fp16\":" << (d.fp16 ? "true" : "false")
                          << ",\"fp64\":" << (d.fp64 ? "true" : "false") << "}";
            }
            std::cout << "]}\n";
        } else {
            std::cout << "OpenCL devices:\n";
            for (const auto& d : devs) {
                std::cout << "  [" << d.type << "] " << d.name << "  (" << d.vendor << ")\n"
                          << "      " << d.version << " | driver " << d.driver << "\n"
                          << "      memory " << (d.global_mem_bytes / 1048576ULL) << " MB"
                          << " | max work-group " << d.max_work_group
                          << " | fp16 " << (d.fp16 ? "yes" : "no")
                          << " | fp64 " << (d.fp64 ? "yes" : "no") << "\n";
            }
            std::string sd = depot::gpu::sim_device_name();
            if (!sd.empty())
                std::cout << "similarity uses: " << sd << "\n";
        }
        return 0;
#else
        std::cerr << "built without OpenCL support (install an OpenCL loader and rebuild)\n";
        return 1;
#endif
    }
    if (cmd == "funnel") {
        std::string branch = "P01-IN01-", model = default_model(), root = ".", seed_path;
        int limit = 50, concurrency = 8, rounds = 1;
        bool dry = false, force = false, all = false, deep = false;
        const char* key = resolve_key();
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--dry") dry = true;
            else if (a == "--force") force = true;
            else if (a == "--branch" && i + 1 < argc) branch = argv[++i];
            else if (a == "--limit" && i + 1 < argc) limit = std::atoi(argv[++i]);
            else if (a == "--concurrency" && i + 1 < argc) {
                concurrency = std::atoi(argv[++i]);
                if (concurrency < 1) concurrency = 1;
            } else if (a == "--model" && i + 1 < argc) model = argv[++i];
            else if (a == "--root" && i + 1 < argc) root = argv[++i];
            else if (a == "--root-node" && i + 1 < argc) seed_path = argv[++i];
            else if (a == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
            else if (a == "--all") all = true;
            else if (a == "--deep") deep = true;
        }
        if (std::string(key).rfind("PASTE", 0) == 0) {
            std::cerr << "no API key: set OPENROUTER_API_KEY (or DEEPSEEK_API_KEY) or edit API_KEY_PLACEHOLDER in main.cpp\n";
            return 1;
        }
        if (rounds < 1) rounds = 1;
        return depot::run_funnel(branch, limit, concurrency, dry, force, model, key, root, nullptr,
                                 seed_path, rounds, all, deep);
    }
    if (cmd == "llm") {
        std::vector<int> levels = {0,1,2,3,4,5,6,7,8,9};
        int limit = 0, offset = 0, concurrency = 8;
        bool dry = false, force = false;
        std::string model = default_model(), root = ".";
        const char* key = resolve_key();
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--dry") dry = true;
            else if (a == "--force") force = true;
            else if (a == "--levels" && i + 1 < argc) {
                levels = depot::parse_levels(argv[++i]);
                if (levels.empty()) {
                    std::cerr << "invalid --levels value (expect digits 0-9, comma-separated)\n";
                    return 2;
                }
            } else if (a == "--limit" && i + 1 < argc) limit = std::atoi(argv[++i]);
            else if (a == "--offset" && i + 1 < argc) offset = std::atoi(argv[++i]);
            else if (a == "--concurrency" && i + 1 < argc) {
                concurrency = std::atoi(argv[++i]);
                if (concurrency < 1) concurrency = 1;
            } else if (a == "--model" && i + 1 < argc) model = argv[++i];
            else if (a == "--root" && i + 1 < argc) root = argv[++i];
        }
        if (std::string(key).rfind("PASTE", 0) == 0) {
            std::cerr << "no API key: set OPENROUTER_API_KEY (or DEEPSEEK_API_KEY) or edit API_KEY_PLACEHOLDER in main.cpp\n";
            return 1;
        }
        return depot::run_llm(levels, limit, offset, concurrency, dry, force, model, key, root);
    }
    std::cerr << "unknown command " << cmd << "\n";
    return 2;
}
