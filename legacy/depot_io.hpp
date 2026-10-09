#pragma once
// depot_io — file/network layer of the Onion data-depot (ingest + llm loop).
#include <functional>
#include <string>
#include <vector>

namespace depot {

// ingest: research.tsv lines are  td<TAB>title[<TAB>sub1|sub2|...]
// root: directory holding the Onion CSVs (default ".").
int run_ingest(const std::string& input, bool dry, const std::string& restrict_prefix,
               const std::string& root = ".", bool use_gpu = false);

// llm: stateless per-node DeepSeek loop over L0..L9.
int run_llm(const std::vector<int>& levels, int limit, int offset, int concurrency,
            bool dry, bool force, const std::string& model, const std::string& key,
            const std::string& root = ".");

// funnel: per node list top 500 -> keep best 400 -> 300 -> LLM-decided final set,
// then nest recursively until items are atomic (hard cap L9).
// raw_ask injects the API transport for tests (nullptr = real curl).
using AskFn = std::function<std::string(const std::string& payload_json)>;
int run_funnel(const std::string& branch, int limit, int concurrency, bool dry,
               bool force, const std::string& model, const std::string& key,
               const std::string& root = ".", AskFn raw_ask = nullptr,
               const std::string& seed_path = "", int rounds = 1, bool all_branches = false,
               bool deep = false);

}  // namespace depot
