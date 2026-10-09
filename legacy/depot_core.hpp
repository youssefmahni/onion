#pragma once
// depot_core — pure algorithmic core of the Onion data-depot (no I/O).
#include <string>
#include <vector>

namespace depot {

struct Row {
    std::string id, path, title;
};

// text utils
std::string trim(const std::string& s);
std::string lower(const std::string& s);
bool ends_with(const std::string& s, const std::string& suf);
std::vector<std::string> split_csv(const std::string& line);
std::vector<std::string> split_pipe(const std::string& s);

// lexical trust (canonicalization + similarity)
std::string sing(const std::string& w);
std::string canonical(const std::string& title);
double lcs_ratio(const std::string& a, const std::string& b);
double jaccard_tokens(const std::string& a, const std::string& b);
double sim_keys(const std::string& k1, const std::string& k2);
std::vector<std::string> style_flags(const std::string& title);

// strict "--levels" parser: returns empty vector if ANY token is invalid or
// out of range (silently mapping garbage to level 0 was the defect).
std::vector<int> parse_levels(const std::string& spec);

// replaces tab/newline with spaces so TSV report columns cannot be broken
// by titles containing them.
std::string sanitize_tsv(const std::string& s);

// ids / paths
int next_tk(const std::vector<Row>& l5, const std::vector<Row>& l6);
std::string td_prefix(const std::string& path);

// llm helpers
std::string json_escape(const std::string& s);
struct FunnelItem {
    std::string title;
    bool atomic = false;
};
// thread-policy math (mirrored by controller.py + dashboard JS fallback):
// grow when idle, shrink when busy, protect RAM/NIC/GPU. Pure & testable.
int policy_target(int cores, double load1, double ram_avail_gb, double net_mbps,
                  double link_mbps, double gpu_pct, int prev);

// funnel parsers: extract {"items": [...]} from an LLM response body.
// r1: items are plain strings (top 500/400/300 candidate lists).
// r4: items are {"title": ..., "atomic": ...} objects (final best set).
std::vector<std::string> parse_funnel_r1(const std::string& body);
std::vector<FunnelItem> parse_funnel_r4(const std::string& body);
struct FunnelCritique {
    double score = 0.0;  // conservative: malformed critique retries, never passes
    std::vector<std::string> remove, add;
    std::string issues;
};
FunnelCritique parse_funnel_c(const std::string& body);
struct LlmAnswer {
    std::string fixed;
    bool valid = true;
    bool admits = true;
    bool parse_ok = false;
    std::string error;
};
LlmAnswer parse_llm(const std::string& body);

// P4: true when a chat-completions body says the reply stopped at max_tokens
// ("finish_reason":"length"). Its JSON is cut off, so the attempt must count
// as failed instead of being parsed into a smaller child set.
bool reply_truncated(const std::string& body);

}  // namespace depot
