#include "checkpoint.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
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
                         {"compacted_before_hlc", compacted_before_hlc},
                         {"compacted_attempts", compacted_attempts},
                         {"compacted_critiques", compacted_critiques},
                         {"compacted_nodes", compacted_nodes},
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

constexpr std::int64_t kNanosPerUsd = 1000000000;
constexpr std::int64_t kHlcLogicalBits = 16;

// Audit costs are stored as decimal strings in the operation log.  The
// compacted snapshot stores their exact sum as integer USD nanounits, so a
// checkpoint never turns accounting into a floating-point approximation.
bool parse_usd_nanos(std::string_view value, std::int64_t* nanos) {
  if (value.empty() || !nanos) return false;
  std::size_t index = 0;
  bool negative = false;
  if (value[index] == '-') {
    negative = true;
    ++index;
  }
  if (index == value.size()) return false;
  std::int64_t whole = 0;
  bool saw_digit = false;
  while (index < value.size() && value[index] != '.') {
    const char ch = value[index++];
    if (ch < '0' || ch > '9') return false;
    saw_digit = true;
    if (whole > (std::numeric_limits<std::int64_t>::max() - (ch - '0')) / 10)
      return false;
    whole = whole * 10 + (ch - '0');
  }
  if (!saw_digit) return false;
  std::int64_t fraction = 0;
  int digits = 0;
  if (index < value.size()) {
    ++index;  // decimal point
    while (index < value.size()) {
      const char ch = value[index++];
      if (ch < '0' || ch > '9' || digits == 9) return false;
      fraction = fraction * 10 + (ch - '0');
      ++digits;
    }
  }
  while (digits++ < 9) fraction *= 10;
  if (whole > (std::numeric_limits<std::int64_t>::max() - fraction) /
                  kNanosPerUsd)
    return false;
  *nanos = whole * kNanosPerUsd + fraction;
  if (negative) *nanos = -*nanos;
  return true;
}

struct CompactionSummary {
  std::int64_t attempts{};
  std::int64_t critiques{};
  std::int64_t passed{};
  std::int64_t tokens_in{};
  std::int64_t tokens_cached{};
  std::int64_t tokens_out{};
  std::int64_t cost_nanos{};
  std::int64_t first_hlc{std::numeric_limits<std::int64_t>::max()};
  std::int64_t last_hlc{};
};

struct CompactionStats {
  std::uint64_t attempts{};
  std::uint64_t critiques{};
  std::uint64_t nodes{};
};

void record_sqlite_error(std::string* error, sqlite3* db,
                         std::string_view prefix) {
  if (error)
    *error = std::string(prefix) + ": " +
             (db ? sqlite3_errmsg(db) : "no database");
}

bool execute(sqlite3* db, std::string_view sql, std::string* error) {
  char* message = nullptr;
  const int result = sqlite3_exec(db, std::string(sql).c_str(), nullptr,
                                  nullptr, &message);
  if (result == SQLITE_OK) return true;
  if (error) {
    *error = message ? message : sqlite3_errmsg(db);
  }
  sqlite3_free(message);
  return false;
}

bool add_checked(std::int64_t* total, std::int64_t value) {
  if (!total ||
      (value > 0 && *total > std::numeric_limits<std::int64_t>::max() - value) ||
      (value < 0 && *total < std::numeric_limits<std::int64_t>::min() - value))
    return false;
  *total += value;
  return true;
}

bool compact_snapshot(const std::filesystem::path& snapshot_path,
                      std::int64_t cutoff_hlc, CompactionStats* stats,
                      std::string* error) {
  if (!stats) {
    if (error) *error = "checkpoint compaction statistics are required";
    return false;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open(snapshot_path.string().c_str(), &db) != SQLITE_OK) {
    record_sqlite_error(error, db, "cannot open checkpoint snapshot");
    if (db) sqlite3_close(db);
    return false;
  }
  const auto close = [&] { sqlite3_close(db); };
  if (!execute(db,
               "CREATE TABLE IF NOT EXISTS checkpoint_summaries ("
               "path TEXT PRIMARY KEY, attempts INTEGER NOT NULL, "
               "critiques INTEGER NOT NULL, passed INTEGER NOT NULL, "
               "tokens_in INTEGER NOT NULL, tokens_cached INTEGER NOT NULL, "
               "tokens_out INTEGER NOT NULL, cost_nanos INTEGER NOT NULL, "
               "first_hlc INTEGER NOT NULL, last_hlc INTEGER NOT NULL);",
               error)) {
    close();
    return false;
  }

  std::map<std::string, CompactionSummary> summaries;
  const auto note_hlc = [](CompactionSummary& summary, std::int64_t hlc) {
    summary.first_hlc = std::min(summary.first_hlc, hlc);
    summary.last_hlc = std::max(summary.last_hlc, hlc);
  };
  sqlite3_stmt* attempts = nullptr;
  if (sqlite3_prepare_v2(
          db,
          "SELECT path, passed, tokens_in, tokens_cached, tokens_out, cost, hlc "
          "FROM attempts WHERE hlc < ?1;",
          -1, &attempts, nullptr) != SQLITE_OK) {
    record_sqlite_error(error, db, "cannot read old attempts");
    close();
    return false;
  }
  sqlite3_bind_int64(attempts, 1, cutoff_hlc);
  int result = SQLITE_ROW;
  while ((result = sqlite3_step(attempts)) == SQLITE_ROW) {
    const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(attempts, 0));
    const auto* cost = reinterpret_cast<const char*>(sqlite3_column_text(attempts, 5));
    if (!path || !cost) {
      sqlite3_finalize(attempts);
      if (error) *error = "old attempt has a missing path or cost";
      close();
      return false;
    }
    CompactionSummary& summary = summaries[path];
    std::int64_t cost_nanos = 0;
    if (!parse_usd_nanos(cost, &cost_nanos) ||
        (cost_nanos > 0 &&
         summary.cost_nanos > std::numeric_limits<std::int64_t>::max() - cost_nanos) ||
        (cost_nanos < 0 &&
         summary.cost_nanos < std::numeric_limits<std::int64_t>::min() - cost_nanos)) {
      sqlite3_finalize(attempts);
      if (error) *error = "old attempt has an invalid or overflowing cost";
      close();
      return false;
    }
    if (!add_checked(&summary.attempts, 1) ||
        !add_checked(&summary.passed, sqlite3_column_int64(attempts, 1)) ||
        !add_checked(&summary.tokens_in, sqlite3_column_int64(attempts, 2)) ||
        !add_checked(&summary.tokens_cached, sqlite3_column_int64(attempts, 3)) ||
        !add_checked(&summary.tokens_out, sqlite3_column_int64(attempts, 4)) ||
        !add_checked(&summary.cost_nanos, cost_nanos)) {
      sqlite3_finalize(attempts);
      if (error) *error = "old attempt totals overflow";
      close();
      return false;
    }
    note_hlc(summary, sqlite3_column_int64(attempts, 6));
    ++stats->attempts;
  }
  sqlite3_finalize(attempts);
  if (result != SQLITE_DONE) {
    record_sqlite_error(error, db, "cannot scan old attempts");
    close();
    return false;
  }

  sqlite3_stmt* critiques = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT path, hlc FROM critiques WHERE hlc < ?1;",
                         -1, &critiques, nullptr) != SQLITE_OK) {
    record_sqlite_error(error, db, "cannot read old critiques");
    close();
    return false;
  }
  sqlite3_bind_int64(critiques, 1, cutoff_hlc);
  result = SQLITE_ROW;
  while ((result = sqlite3_step(critiques)) == SQLITE_ROW) {
    const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(critiques, 0));
    if (!path) {
      sqlite3_finalize(critiques);
      if (error) *error = "old critique has a missing path";
      close();
      return false;
    }
    CompactionSummary& summary = summaries[path];
    if (!add_checked(&summary.critiques, 1)) {
      sqlite3_finalize(critiques);
      if (error) *error = "old critique totals overflow";
      close();
      return false;
    }
    note_hlc(summary, sqlite3_column_int64(critiques, 1));
    ++stats->critiques;
  }
  sqlite3_finalize(critiques);
  if (result != SQLITE_DONE) {
    record_sqlite_error(error, db, "cannot scan old critiques");
    close();
    return false;
  }

  if (!execute(db, "BEGIN IMMEDIATE;", error)) {
    close();
    return false;
  }
  const auto rollback = [&] { execute(db, "ROLLBACK;", nullptr); };
  sqlite3_stmt* insert = nullptr;
  const char* insert_sql =
      "INSERT INTO checkpoint_summaries("
      "path,attempts,critiques,passed,tokens_in,tokens_cached,tokens_out,"
      "cost_nanos,first_hlc,last_hlc) VALUES(?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(path) DO UPDATE SET "
      "attempts=checkpoint_summaries.attempts+excluded.attempts,"
      "critiques=checkpoint_summaries.critiques+excluded.critiques,"
      "passed=checkpoint_summaries.passed+excluded.passed,"
      "tokens_in=checkpoint_summaries.tokens_in+excluded.tokens_in,"
      "tokens_cached=checkpoint_summaries.tokens_cached+excluded.tokens_cached,"
      "tokens_out=checkpoint_summaries.tokens_out+excluded.tokens_out,"
      "cost_nanos=checkpoint_summaries.cost_nanos+excluded.cost_nanos,"
      "first_hlc=MIN(checkpoint_summaries.first_hlc,excluded.first_hlc),"
      "last_hlc=MAX(checkpoint_summaries.last_hlc,excluded.last_hlc);";
  if (sqlite3_prepare_v2(db, insert_sql, -1, &insert, nullptr) != SQLITE_OK) {
    record_sqlite_error(error, db, "cannot prepare checkpoint summary");
    rollback();
    close();
    return false;
  }
  for (const auto& [path, summary] : summaries) {
    sqlite3_bind_text(insert, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert, 2, summary.attempts);
    sqlite3_bind_int64(insert, 3, summary.critiques);
    sqlite3_bind_int64(insert, 4, summary.passed);
    sqlite3_bind_int64(insert, 5, summary.tokens_in);
    sqlite3_bind_int64(insert, 6, summary.tokens_cached);
    sqlite3_bind_int64(insert, 7, summary.tokens_out);
    sqlite3_bind_int64(insert, 8, summary.cost_nanos);
    sqlite3_bind_int64(insert, 9, summary.first_hlc);
    sqlite3_bind_int64(insert, 10, summary.last_hlc);
    if (sqlite3_step(insert) != SQLITE_DONE) {
      sqlite3_finalize(insert);
      record_sqlite_error(error, db, "cannot write checkpoint summary");
      rollback();
      close();
      return false;
    }
    sqlite3_reset(insert);
    sqlite3_clear_bindings(insert);
  }
  sqlite3_finalize(insert);
  if (!execute(db, "DELETE FROM attempts WHERE hlc < " +
                       std::to_string(cutoff_hlc) + ";",
               error) ||
      !execute(db, "DELETE FROM critiques WHERE hlc < " +
                       std::to_string(cutoff_hlc) + ";",
               error) ||
      !execute(db, "COMMIT;", error)) {
    rollback();
    close();
    return false;
  }
  stats->nodes = summaries.size();
  // Reclaim the deleted audit pages in the compacted copy, never in the
  // live replayable view.
  const bool vacuumed = execute(db, "VACUUM;", error);
  close();
  return vacuumed;
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

  // M8: a checkpoint is a compact, self-contained replica.  The live
  // SQLite view and immutable signed segments keep full audit history for
  // replay; this snapshot turns records older than 30 days into exact
  // per-node totals before a storage-constrained joiner receives it.
  const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
  constexpr std::int64_t kRetentionMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::hours(24 * 30)).count();
  const std::int64_t cutoff_hlc = (now_ms - kRetentionMs) << kHlcLogicalBits;
  CompactionStats compaction;
  if (!compact_snapshot(snapshot_path, cutoff_hlc, &compaction, error))
    return false;

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
    manifest.compacted_before_hlc = static_cast<std::uint64_t>(cutoff_hlc);
    manifest.compacted_attempts = compaction.attempts;
    manifest.compacted_critiques = compaction.critiques;
    manifest.compacted_nodes = compaction.nodes;
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
