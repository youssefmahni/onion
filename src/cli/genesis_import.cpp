#include "genesis_import.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/depot_core.hpp"
#include "log/log.hpp"
#include "platform/platform.hpp"
#include "sched/sched.hpp"
#include "state/state.hpp"
#include "store/store.hpp"
#include "store/checkpoint.hpp"

namespace {

using onion::log::Hlc;
using onion::log::Record;

Record make_record(std::string type, nlohmann::json payload, Hlc hlc) {
  Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  record.hlc = hlc;
  return record;
}

std::string flag(int argc, char** argv, const std::string& name,
                 const std::string& fallback) {
  for (int index = 0; index < argc; ++index)
    if (std::string(argv[index]) == name && index + 1 < argc)
      return argv[index + 1];
  return fallback;
}

bool has_flag(int argc, char** argv, const std::string& name) {
  for (int index = 0; index < argc; ++index)
    if (std::string(argv[index]) == name) return true;
  return false;
}

struct CorpusRow {
  std::string code;
  std::string path;
  std::string title;
  int layer{};
};

// One row per path, plus the duplicates seen for it (last row wins).
struct Corpus {
  std::vector<CorpusRow> rows;
  std::map<std::string, std::vector<std::string>> duplicates;  // path -> titles
};

Corpus read_corpus(const std::filesystem::path& root) {
  Corpus corpus;
  std::map<std::string, std::string> kept_title;
  for (int level = 0; level <= 9; ++level) {
    char name[64];
    std::snprintf(name, sizeof(name), "Onion - level_%02d.csv", level);
    std::ifstream in(root / name);
    if (!in) continue;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
      if (depot::trim(line).empty()) continue;
      const auto columns = depot::split_csv(line);
      CorpusRow row;
      row.layer = level;
      if (level == 0) {
        if (columns.size() < 2) continue;
        row.code = depot::trim(columns[0]);
        row.path = depot::trim(columns[0]);
        row.title = depot::trim(columns[1]);
      } else {
        if (columns.size() < 3) continue;
        row.code = depot::trim(columns[0]);
        row.path = depot::trim(columns[1]);
        row.title = depot::trim(columns[2]);
      }
      if (row.path.empty()) continue;
      auto kept = kept_title.find(row.path);
      if (kept != kept_title.end()) {
        if (kept->second != row.title)
          corpus.duplicates[row.path].push_back(kept->second);
        else if (corpus.duplicates.find(row.path) == corpus.duplicates.end())
          corpus.duplicates[row.path] = {};
      }
      kept_title[row.path] = row.title;
      corpus.rows.push_back(std::move(row));
    }
  }
  // Drop earlier duplicates of the last kept row.
  std::vector<CorpusRow> deduped;
  std::set<std::string> seen;
  for (auto it = corpus.rows.rbegin(); it != corpus.rows.rend(); ++it) {
    if (seen.insert(it->path).second) deduped.push_back(*it);
  }
  std::reverse(deduped.begin(), deduped.end());
  corpus.rows = std::move(deduped);
  return corpus;
}

// The proposal lists every duplicated path with its candidate titles and a
// recommended keeper; the resolution file (owner-approved) holds
// path<TAB>kept_title lines. The import refuses to run without one.
bool write_proposal(const Corpus& corpus, const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "path\tkept\tcandidates\n";
  for (const auto& [dup_path, titles] : corpus.duplicates) {
    std::string candidates;
    for (std::size_t i = 0; i < titles.size(); ++i)
      candidates += (i ? "|" : "") + titles[i];
    out << dup_path << "\t" << depot::sanitize_tsv(titles.empty() ? "" : titles.back())
        << "\t" << depot::sanitize_tsv(candidates) << "\n";
  }
  return static_cast<bool>(out);
}

}  // namespace

int genesis_import_cmd(int argc, char** argv) {
  const std::string root = flag(argc, argv, "--root", "");
  const std::string data_dir = flag(argc, argv, "--data-dir", "");
  if (root.empty() || data_dir.empty()) {
    std::cerr << "usage: data_depot genesis-import --root <corpus-copy> "
                 "--data-dir <dir> [--propose <file>] [--resolution <file>]\n";
    return 2;
  }
  const Corpus corpus = read_corpus(root);

  if (has_flag(argc, argv, "--propose")) {
    const std::string proposal = flag(argc, argv, "--propose", "");
    if (proposal.empty()) {
      std::cerr << "--propose needs a file path\n";
      return 2;
    }
    if (!write_proposal(corpus, proposal)) {
      std::cerr << "cannot write the proposal\n";
      return 1;
    }
    std::cout << "proposal written to " << proposal << " ("
              << corpus.duplicates.size() << " duplicated paths)\n";
    return 0;
  }

  const std::string resolution = flag(argc, argv, "--resolution", "");
  if (!corpus.duplicates.empty() && resolution.empty()) {
    std::cerr << "duplicate paths found (" << corpus.duplicates.size()
              << "): run with --propose first, approve the resolution, and "
                 "pass --resolution\n";
    return 2;
  }
  std::set<std::string> resolved;
  if (!resolution.empty()) {
    std::ifstream in(resolution);
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
      const std::size_t tab = line.find('\t');
      if (tab != std::string::npos) resolved.insert(line.substr(0, tab));
    }
  }

  std::string error;
  onion::platform::KeyStore key_store(data_dir + "/keys");
  const auto stored = key_store.get("device-key");
  if (!stored) {
    std::cerr << "no device key: run 'node run' once first\n";
    return 1;
  }
  auto key = std::make_shared<onion::log::DeviceKey>(
      onion::log::DeviceKey::from_seed_hex(*stored));
  onion::state::StateWriter::Options options;
  options.data_dir = data_dir;
  options.origin = key->device_id;
  options.device_key = key;
  options.signer = std::make_unique<onion::log::Ed25519Signer>(*key);
  auto writer = std::make_unique<onion::state::StateWriter>(std::move(options));
  if (!writer->start(&error)) {
    std::cerr << error << "\n";
    return 1;
  }

  // Genesis import: ADD_NODE records in layer order, legacy ids in slot 0.
  std::vector<Record> records;
  Hlc hlc = 1;
  for (const CorpusRow& row : corpus.rows) {
    records.push_back(make_record(
        "ADD_NODE",
        {{"path", row.path},
         {"layer", row.layer},
         {"title", row.title},
         {"legacy", true}},
        hlc++));
    if (records.size() >= 500) {
      writer->submit(onion::state::CommitTerminal{std::move(records)});
      records.clear();
      writer->sync();
    }
  }
  if (!records.empty()) {
    writer->submit(onion::state::CommitTerminal{std::move(records)});
    writer->sync();
  }

  // Legacy settles and atomic flags (funnel_state.tsv / atomic_items.tsv).
  {
    std::ifstream state(root + "/funnel_state.tsv");
    std::string line;
    std::getline(state, line);
    std::vector<Record> settles;
    Hlc settle_hlc = hlc;
    while (std::getline(state, line)) {
      const auto columns = depot::split_csv(line);
      if (columns.size() < 2) continue;
      const std::string path = depot::trim(columns[0]);
      settles.push_back(make_record(
          "SETTLE",
          {{"path", path},
           {"kept", columns.size() > 1 ? std::atoi(columns[1].c_str()) : 0},
           {"atomic", columns.size() > 2 ? std::atoi(columns[2].c_str()) : 0},
           {"attempts",
            columns.size() > 3 ? std::atoi(columns[3].c_str()) : 0},
           {"score", columns.size() > 4 ? depot::trim(columns[4]) : "0"},
           {"end", "children"},
           {"legacy", true}},
          settle_hlc++));
    }
    if (!settles.empty()) {
      writer->submit(onion::state::CommitTerminal{std::move(settles)});
      writer->sync();
    }
  }
  {
    std::ifstream atomic(root + "/atomic_items.tsv");
    std::string line;
    std::getline(atomic, line);
    std::vector<Record> atomics;
    Hlc atomic_hlc = hlc;
    while (std::getline(atomic, line)) {
      const auto columns = depot::split_csv(line);
      if (columns.empty()) continue;
      atomics.push_back(make_record(
          "ATOMIC", {{"path", depot::trim(columns[0])}, {"verified", true},
                     {"legacy", true}},
          atomic_hlc++));
    }
    if (!atomics.empty()) {
      writer->submit(onion::state::CommitTerminal{std::move(atomics)});
      writer->sync();
    }
  }

  // Verifier: counts per layer, parents present, Merkle root against a
  // fresh CSV read.
  std::map<int, int> store_layers;
  std::set<std::string> store_paths;
  std::vector<onion::store::Node> all_nodes = writer->db().all_nodes(&error);
  for (const auto& node : all_nodes) {
    store_layers[node.layer]++;
    store_paths.insert(node.path);
  }
  std::map<int, int> csv_layers;
  for (const CorpusRow& row : corpus.rows) csv_layers[row.layer]++;
  bool verified = true;
  for (int layer = 0; layer <= 9; ++layer) {
    if (csv_layers[layer] != store_layers[layer]) {
      std::cout << "count mismatch at layer " << layer << ": csv "
                << csv_layers[layer] << " store " << store_layers[layer]
                << "\n";
      verified = false;
    }
  }
  int orphans = 0;
  for (const CorpusRow& row : corpus.rows) {
    const std::size_t dash = row.path.rfind('-');
    const std::string parent =
        dash == std::string::npos ? "" : row.path.substr(0, dash);
    if (!parent.empty() && !store_paths.count(parent)) {
      if (orphans < 10)
        std::cout << "orphan: " << row.path << " (parent " << parent
                  << " missing)\n";
      orphans++;
    }
  }
  const std::string root_merkle = onion::store::merkle_root(all_nodes);
  std::vector<onion::store::Node> csv_nodes;
  // The import stamps each record's HLC with its position in the import
  // order, so the CSV side reproduces the same leaves deterministically.
  std::uint64_t csv_hlc = 1;
  for (const CorpusRow& row : corpus.rows) {
    onion::store::Node node;
    node.code = row.code;
    node.path = row.path;
    node.layer = row.layer;
    node.title = row.title;
    node.hlc = csv_hlc++;
    csv_nodes.push_back(std::move(node));
  }
  const std::string csv_merkle = onion::store::merkle_root(csv_nodes);
  if (root_merkle != csv_merkle) {
    std::cout << "merkle mismatch: store " << root_merkle << " csv "
              << csv_merkle << "\n";
    verified = false;
  }
  std::cout << "imported " << corpus.rows.size() << " nodes, "
            << corpus.duplicates.size() << " duplicated paths resolved, "
            << "layers: ";
  for (int layer = 0; layer <= 9 && csv_layers.count(layer); ++layer)
    std::cout << "L" << layer << "=" << csv_layers[layer] << " ";
  std::cout << "\n";
  std::cout << "orphans=" << orphans << " merkle=" << root_merkle << "\n";
  std::cout << (verified ? "VERIFIED" : "VERIFICATION FAILED") << "\n";
  writer->stop();
  return verified ? 0 : 1;
}
