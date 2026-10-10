#include "checkpoint.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "../log/log.hpp"
#include "sqlite3.h"

namespace onion::store {

std::string merkle_root(const std::vector<Node>& nodes) {
  std::vector<Node> sorted = nodes;
  std::sort(sorted.begin(), sorted.end(),
            [](const Node& a, const Node& b) { return a.path < b.path; });
  std::vector<std::string> level;
  level.reserve(sorted.size());
  for (const Node& node : sorted) {
    const nlohmann::json leaf = {
        {"code", node.code},       {"path", node.path},
        {"layer", node.layer},     {"title", node.title},
        {"hlc", node.hlc},         {"tombstone", node.tombstone}};
    const std::string canonical = onion::log::canonical_json(leaf);
    level.push_back(onion::log::blake2b_hex(canonical));
  }
  while (level.size() > 1) {
    std::vector<std::string> next;
    next.reserve((level.size() + 1) / 2);
    for (std::size_t i = 0; i < level.size(); i += 2) {
      if (i + 1 < level.size())
        next.push_back(onion::log::blake2b_hex(level[i] + level[i + 1]));
      else
        next.push_back(level[i]);  // odd tail promoted
    }
    level = std::move(next);
  }
  return level.empty() ? onion::log::blake2b_hex(std::string{}) : level[0];
}

std::string CheckpointManifest::to_json() const {
  nlohmann::json json = {{"hlc", hlc},
                         {"snapshot_hash", snapshot_hash},
                         {"merkle_root", merkle_root},
                         {"vector", nlohmann::json::object()},
                         {"signature", signature},
                         {"cosignature", cosignature}};
  for (const auto& [origin, seq] : vector) json["vector"][origin] = seq;
  return json.dump();
}

CheckpointWriter::CheckpointWriter(std::filesystem::path dir,
                                   std::function<std::string(std::string_view)> signer)
    : dir_(std::move(dir)), signer_(std::move(signer)) {}

namespace {
// BLAKE2b-256 of a file's bytes, hex.
std::string file_hash(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  return onion::log::blake2b_hex(content);
}
}  // namespace

bool CheckpointWriter::write(std::uint64_t hlc,
                             const std::map<std::string, std::int64_t>& vector,
                             std::string* error) {
  // VACUUM INTO needs a connection; open the store read-only-ish and run it.
  sqlite3* handle = nullptr;
  if (sqlite3_open((dir_ / "onion.db").string().c_str(), &handle) != SQLITE_OK) {
    if (error) *error = "cannot open the store for checkpointing";
    if (handle) sqlite3_close(handle);
    return false;
  }
  const std::string snapshot_name = "checkpoint-" + std::to_string(hlc) + ".db";
  const std::filesystem::path snapshot_path = dir_ / snapshot_name;
  std::remove(snapshot_path.string().c_str());
  char* errmsg = nullptr;
  // VACUUM INTO resolves relative to the process CWD: use the absolute path.
  std::string sql = "VACUUM INTO '" + snapshot_path.string() + "';";
  if (sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
    if (error) *error = std::string("VACUUM INTO failed: ") +
                        (errmsg ? errmsg : "unknown");
    sqlite3_free(errmsg);
    sqlite3_close(handle);
    return false;
  }
  sqlite3_close(handle);

  // Merkle root from the store's nodes.
  {
    Db db(dir_ / "onion.db");
    std::string open_error;
    if (!db.open(&open_error)) {
      if (error) *error = open_error;
      return false;
    }
    std::string query_error;
    const std::vector<Node> nodes = db.all_nodes(&query_error);
    CheckpointManifest manifest;
    manifest.hlc = hlc;
    manifest.snapshot_hash = file_hash(snapshot_path);
    manifest.merkle_root = merkle_root(nodes);
    manifest.vector = vector;
    manifest.signature = "";
    manifest.cosignature = "";
    const std::string canonical = onion::log::canonical_json(
        nlohmann::json::parse(manifest.to_json()));
    manifest.signature = signer_ ? signer_(canonical) : std::string{};
    std::ofstream out(dir_ / (snapshot_name + ".manifest"),
                      std::ios::binary);
    if (!out) {
      if (error) *error = "cannot write the manifest";
      return false;
    }
    out << manifest.to_json() << "\n";
    out.close();
  }

  // Keep the last 3 checkpoints.
  std::vector<std::filesystem::path> snapshots;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("checkpoint-", 0) == 0 &&
        name.size() >= 3 && name.substr(name.size() - 3) == ".db")
      snapshots.push_back(entry.path());
  }
  std::sort(snapshots.begin(), snapshots.end());
  while (snapshots.size() > 3) {
    const std::filesystem::path oldest = snapshots.front();
    std::filesystem::remove(oldest, ec);
    std::filesystem::remove(std::filesystem::path(oldest.string() + ".manifest"), ec);
    snapshots.erase(snapshots.begin());
  }
  return true;
}

}  // namespace onion::store
