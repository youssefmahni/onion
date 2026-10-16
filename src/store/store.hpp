#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace onion::store {
inline constexpr std::string_view component_name{"store"};

// Schema version stamped into schema_version; bumping it selects a migration.
inline constexpr int kSchemaVersion = 1;

struct Node {
  std::int64_t id{};
  std::string code;
  std::string path;
  std::int64_t parent_id{};
  int layer{};
  std::string title;
  std::string origin;
  std::uint64_t hlc{};
  int tombstone{};
  // Decision 0027: how the node entered the tree — "import" (corpus
  // import, exempt from the allowance fold per decision 0025) or
  // "engine" (created by a campaign settle). Empty = engine (legacy).
  std::string source;
  // Decision 0027: the domain tree this node belongs to. Empty = the
  // planning tree (the embedded default profile). Campaigns, exports
  // and the dashboard filter by tree, and node codes may repeat across
  // different trees.
  std::string tree;
};

struct Settle {
  std::string path;
  int kept{};
  int atomic{};
  int attempts{};
  std::string score;
  std::string campaign;
  std::string end;  // how the node ended ("" for pre-migration rows)
  std::uint64_t hlc{};
  std::string origin;
};

struct Attempt {
  std::string path;
  int n{};
  std::string score;
  int passed{};
  std::int64_t tokens_in{};
  std::int64_t tokens_cached{};
  std::int64_t tokens_out{};
  std::string cost;
  std::string campaign;
  std::uint64_t hlc{};
  std::string origin;
};

struct Critique {
  std::string path;
  std::string score;
  std::string issues;
  std::string remove;
  std::string add;
  std::uint64_t hlc{};
  std::string origin;
};

struct Review {
  std::string path;
  std::string reason;
  std::string state;  // open | decided
  std::string note;
  int atomic{};  // atomic children stored with the review (W5)
  std::uint64_t hlc{};
  std::string origin;
};

struct Campaign {
  std::string id;
  std::string spec;
  std::string state;  // running | paused | stopped
  std::uint64_t hlc{};
  std::string origin;
  std::string reason;  // e.g. "no progress" (the churn guard, decision 0025)
};

// Decision 0027: a folded domain profile (PROFILE records). Content-
// addressed: the hash is the profile_hash over the canonical JSON, and
// campaigns carry it.
struct DomainProfile {
  std::string hash;
  std::string name;
  std::string tree;
  std::string json;
};

// RAII prepared statement: fails hard on misuse in debug, reports errors out.
class Statement {
 public:
  Statement() = default;
  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&& other) noexcept;

  sqlite3_stmt* handle() const { return stmt_; }

  bool bind(int index, std::string_view value, std::string* error);
  bool bind(int index, std::int64_t value, std::string* error);
  bool step(std::string* error);          // false at SQLITE_DONE
  bool step_row(std::string* error);      // false at SQLITE_DONE, error->set
  void reset(std::string* error);

  std::string text(int column) const;
  std::string blob(int column) const;  // raw bytes (BLOB-safe)
  int column_count() const;
  std::int64_t integer(int column) const;
  int integer_int(int column) const;

 private:
  friend class Db;
  explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
  sqlite3_stmt* stmt_{};
};

// Single SQLite database in WAL mode, schema exactly as DESIGN.md › Storage
// plus schema_version. Statements are prepared per call; heavy callers
// should prepare once and reuse.
class Db {
 public:
  explicit Db(std::filesystem::path path);
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  Db(Db&& other) noexcept;
  Db& operator=(Db&& other) noexcept;

  // Creates the schema (idempotent) and applies pending migrations.
  bool open(std::string* error);
  // Opens WITHOUT schema creation or migration, for reader threads: one
  // read-only connection per thread, never shared (a WAL reader never
  // blocks on the writer's transactions). Fails when the file or its WAL
  // sidecars are unavailable.
  bool open_readonly(std::string* error);
  // Closes the handle so the file can be deleted (needed on Windows, where
  // an open database keeps its files locked).
  void close();
  // The raw handle, for callers that need sqlite3 functions directly
  // (e.g. sqlite3_changes).
  sqlite3* handle() const { return handle_; }
  // Rows changed by the most recent statement on this connection
  // (sqlite3_changes; the fold's counter deltas read this).
  int changes() const;

  // Plain SQL with no results.
  bool exec(std::string_view sql, std::string* error);
  // Prepare a statement for reuse.
  std::optional<Statement> prepare(std::string_view sql, std::string* error) const;

  // Typed helpers.
  std::optional<Node> node_by_path(std::string_view path, std::string* error) const;
  std::vector<Node> nodes_in_subtree(std::string_view path, std::string* error) const;
  std::vector<Node> nodes_at_layer(int layer, std::string* error,
                                   std::string_view tree = "") const;
  std::vector<Node> all_nodes(std::string* error) const;
  std::vector<Node> nodes_with_path_prefix(std::string_view prefix,
                                           std::string* error) const;
  // Dashboard tree reads (plan §4.1): keyset cursors, never OFFSET.
  std::vector<Node> children_page(std::string_view parent_path,
                                  std::string_view cursor, int limit,
                                  std::string* error) const;
  std::int64_t children_count(std::string_view parent_path,
                              std::string* error) const;
  std::int64_t children_count_after(std::string_view parent_path,
                                    std::string_view after_path,
                                    std::string* error) const;
  std::int64_t subtree_count(std::string_view path, std::string* error) const;
  std::optional<Settle> settle_by_path(std::string_view path,
                                       std::string* error) const;
  std::vector<Attempt> attempts_for(std::string_view path,
                                    std::string* error) const;
  std::vector<Critique> critiques_for(std::string_view path,
                                      std::string* error) const;
  std::vector<std::pair<int, std::int64_t>> level_counts(
      std::string* error) const;
  std::vector<std::pair<std::string, std::int64_t>> status_counts(
      std::string* error) const;
  // LIKE-based title search (the viewcache FTS path supersedes this when
  // present). At most `limit` rows, best matches by prefix first.
  std::vector<Node> search_titles(std::string_view query, int limit,
                                  std::string* error) const;
  // The membership state ("ACTIVE" | ...) for a device id, if known.
  std::optional<std::string> member_state(std::string_view device_id,
                                          std::string* error) const;
  std::vector<Review> open_reviews(std::string* error) const;
  // The review row's state for one path ("open" | "decided"), empty when
  // the path never had a review (M8 depth checkpoints: a decided review
  // means the pause was released and the node must be attempted).
  std::optional<std::string> review_state(std::string_view path,
                                          std::string* error) const;
  std::vector<Campaign> campaigns(std::string* error) const;
  std::vector<Settle> settles(std::string* error) const;
  // All ATTEMPT audit rows, for measured cost parameters (decision 0016).
  std::vector<Attempt> attempts(std::string* error) const;
  std::int64_t max_node_id(std::string* error) const;
  // The largest numeric TK id value (TK<digits>) in nodes.
  std::int64_t max_tk_value(std::string* error) const;
  std::string spend_usd(std::string* error) const;
  // Recent observed spend extrapolated to USD/hour.  The HLC physical
  // component is the wall-clock millisecond timestamp, so this remains
  // portable across platforms and replay order.
  std::string spend_rate_usd_per_hour(std::int64_t window_seconds,
                                      std::string* error) const;
  // Exports layers 0..max_layer (≤ 9) as "Onion - level_NN.csv" in dir.
  bool export_csv(const std::filesystem::path& dir, int max_layer,
                  std::string* error, std::string_view tree = "") const;

  // Decision 0027: folded domain profiles, content-addressed by hash.
  std::optional<DomainProfile> domain_profile(std::string_view hash,
                                              std::string* error) const;
  bool store_domain_profile(std::string_view hash, std::string_view name,
                            std::string_view tree, std::string_view json,
                            std::string* error);

  std::filesystem::path path() const { return path_; }

 private:
  std::filesystem::path path_;
  sqlite3* handle_{};
};

}  // namespace onion::store
