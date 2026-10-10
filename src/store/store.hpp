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
};

struct Settle {
  std::string path;
  int kept{};
  int atomic{};
  int attempts{};
  std::string score;
  std::string campaign;
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
  std::uint64_t hlc{};
  std::string origin;
};

struct Campaign {
  std::string id;
  std::string spec;
  std::string state;  // running | paused | stopped
  std::uint64_t hlc{};
  std::string origin;
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

  // Plain SQL with no results.
  bool exec(std::string_view sql, std::string* error);
  // Prepare a statement for reuse.
  std::optional<Statement> prepare(std::string_view sql, std::string* error) const;

  // Typed helpers.
  std::optional<Node> node_by_path(std::string_view path, std::string* error) const;
  std::vector<Node> nodes_in_subtree(std::string_view path, std::string* error) const;
  std::vector<Node> nodes_at_layer(int layer, std::string* error) const;
  std::vector<Node> all_nodes(std::string* error) const;
  std::vector<Node> nodes_with_path_prefix(std::string_view prefix,
                                           std::string* error) const;
  std::vector<Review> open_reviews(std::string* error) const;
  std::vector<Campaign> campaigns(std::string* error) const;
  std::vector<Settle> settles(std::string* error) const;
  std::int64_t max_node_id(std::string* error) const;
  std::string spend_usd(std::string* error) const;
  // Exports layers 0..max_layer (≤ 9) as "Onion - level_NN.csv" in dir.
  bool export_csv(const std::filesystem::path& dir, int max_layer,
                  std::string* error) const;

  std::filesystem::path path() const { return path_; }

 private:
  std::filesystem::path path_;
  sqlite3* handle_{};
};

}  // namespace onion::store
