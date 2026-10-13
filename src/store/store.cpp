#include "store.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <sqlite3.h>

namespace onion::store {
namespace {

std::string csv_field(std::string_view value) {
  const bool needs_quoting =
      value.find(',') != std::string_view::npos ||
      value.find('"') != std::string_view::npos ||
      value.find('\n') != std::string_view::npos ||
      value.find('\r') != std::string_view::npos;
  if (!needs_quoting) return std::string(value);
  std::string out;
  out.reserve(value.size() + 2);
  out += '"';
  for (const char c : value) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
  return out;
}

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS nodes (
  id        INTEGER PRIMARY KEY,
  code      TEXT NOT NULL,
  path      TEXT NOT NULL UNIQUE,
  parent_id INTEGER,
  layer     INTEGER NOT NULL,
  title     TEXT NOT NULL,
  origin    TEXT NOT NULL,
  hlc       INTEGER NOT NULL,
  tombstone INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS nodes_parent ON nodes(parent_id);
CREATE TABLE IF NOT EXISTS settles   (path TEXT PRIMARY KEY, kept INT, atomic INT, attempts INT, score TEXT,
                        campaign TEXT, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS attempts  (path TEXT, n INT, score TEXT, passed INT, tokens_in INT, tokens_cached INT,
                        tokens_out INT, cost TEXT, campaign TEXT, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS critiques (path TEXT, score TEXT, issues TEXT, remove TEXT, add_ TEXT, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS reviews   (path TEXT PRIMARY KEY, reason TEXT, state TEXT, note TEXT, atomic INT NOT NULL DEFAULT 0, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS campaigns (id TEXT PRIMARY KEY, spec TEXT, state TEXT, hlc INT, origin TEXT);
CREATE TABLE IF NOT EXISTS members   (device_id TEXT PRIMARY KEY, pubkey BLOB, slot INT, state TEXT, incarnation INT,
                        profile_version INT, evict_cutoff INT);
CREATE TABLE IF NOT EXISTS profiles  (device_id TEXT, version INT, prev TEXT, reason TEXT, changed TEXT,
                        spec TEXT, rules_version INT, schema INT, hlc INT,
                        PRIMARY KEY (device_id, version));
CREATE TABLE IF NOT EXISTS applied   (origin TEXT PRIMARY KEY, seq INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
)sql";

void set_error(std::string* error, sqlite3* handle, std::string_view prefix) {
  if (!error) return;
  std::ostringstream out;
  out << prefix << ": " << (handle ? sqlite3_errmsg(handle) : "no database");
  *error = out.str();
}

}  // namespace

Statement::~Statement() {
  if (stmt_) sqlite3_finalize(stmt_);
}
Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
  other.stmt_ = nullptr;
}
Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    if (stmt_) sqlite3_finalize(stmt_);
    stmt_ = other.stmt_;
    other.stmt_ = nullptr;
  }
  return *this;
}

bool Statement::bind(int index, std::string_view value, std::string* error) {
  const int rc = sqlite3_bind_text(stmt_, index, value.data(),
                                   static_cast<int>(value.size()),
                                   SQLITE_TRANSIENT);
  if (rc != SQLITE_OK && error)
    *error = std::string("sqlite bind failed: ") + sqlite3_errstr(rc);
  return rc == SQLITE_OK;
}

bool Statement::bind(int index, std::int64_t value, std::string* error) {
  const int rc = sqlite3_bind_int64(stmt_, index, value);
  if (rc != SQLITE_OK && error)
    *error = std::string("sqlite bind failed: ") + sqlite3_errstr(rc);
  return rc == SQLITE_OK;
}

bool Statement::step(std::string* error) {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW || rc == SQLITE_DONE) return rc == SQLITE_ROW;
  if (error) *error = std::string("sqlite step failed: ") + sqlite3_errstr(rc);
  return false;
}

bool Statement::step_row(std::string* error) { return step(error); }

void Statement::reset(std::string* error) {
  const int rc = sqlite3_reset(stmt_);
  if (rc != SQLITE_OK && error)
    *error = std::string("sqlite reset failed: ") + sqlite3_errstr(rc);
}

int Statement::column_count() const {
  return stmt_ ? sqlite3_column_count(stmt_) : 0;
}

std::string Statement::text(int column) const {
  const auto* value = sqlite3_column_text(stmt_, column);
  if (!value) return {};
  return reinterpret_cast<const char*>(value);
}

std::int64_t Statement::integer(int column) const {
  return sqlite3_column_int64(stmt_, column);
}

int Statement::integer_int(int column) const {
  return sqlite3_column_int(stmt_, column);
}

Db::Db(std::filesystem::path path) : path_(std::move(path)) {}
Db::~Db() { close(); }

void Db::close() {
  if (handle_) {
    sqlite3_close(handle_);
    handle_ = nullptr;
  }
}
Db::Db(Db&& other) noexcept : path_(std::move(other.path_)), handle_(other.handle_) {
  other.handle_ = nullptr;
}
Db& Db::operator=(Db&& other) noexcept {
  if (this != &other) {
    close();
    path_ = std::move(other.path_);
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool Db::open(std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(path_.parent_path(), ec);
  if (ec) {
    set_error(error, nullptr, "cannot create data directory");
    return false;
  }
  if (sqlite3_open(path_.string().c_str(), &handle_) != SQLITE_OK) {
    set_error(error, handle_, "cannot open database");
    return false;
  }
  sqlite3_busy_timeout(handle_, 5000);
  char* errmsg = nullptr;
  if (sqlite3_exec(handle_, "PRAGMA journal_mode=WAL;", nullptr, nullptr,
                   &errmsg) != SQLITE_OK) {
    set_error(error, handle_, "cannot enable WAL");
    sqlite3_free(errmsg);
    return false;
  }
  sqlite3_free(errmsg);
  if (sqlite3_exec(handle_, kSchema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
    set_error(error, handle_, "cannot create schema");
    sqlite3_free(errmsg);
    return false;
  }
  sqlite3_free(errmsg);

  // Migration (C4): the attempts fold is idempotent, keyed by the record's
  // own identity (origin, hlc) — never (path, attempt number), so two
  // devices colliding on one ticket both count their spend.  Older builds
  // could double-fold a replayed durable attempt batch; remove those
  // duplicates before the unique index exists, on every open (idempotent).
  if (sqlite3_exec(handle_,
                   "DELETE FROM attempts WHERE rowid NOT IN "
                   "(SELECT MIN(rowid) FROM attempts GROUP BY origin, hlc);",
                   nullptr, nullptr, &errmsg) != SQLITE_OK) {
    set_error(error, handle_, "cannot dedup attempts");
    sqlite3_free(errmsg);
    return false;
  }
  sqlite3_free(errmsg);
  if (sqlite3_exec(handle_,
                   "CREATE UNIQUE INDEX IF NOT EXISTS attempts_once "
                   "ON attempts(origin, hlc);",
                   nullptr, nullptr, &errmsg) != SQLITE_OK) {
    set_error(error, handle_, "cannot create attempts index");
    sqlite3_free(errmsg);
    return false;
  }
  sqlite3_free(errmsg);
  if (sqlite3_exec(handle_,
                   "DELETE FROM critiques WHERE rowid NOT IN "
                   "(SELECT MIN(rowid) FROM critiques GROUP BY origin, hlc);",
                   nullptr, nullptr, &errmsg) != SQLITE_OK) {
    set_error(error, handle_, "cannot dedup critiques");
    sqlite3_free(errmsg);
    return false;
  }
  sqlite3_free(errmsg);
  if (sqlite3_exec(handle_,
                   "CREATE UNIQUE INDEX IF NOT EXISTS critiques_once "
                   "ON critiques(origin, hlc);",
                   nullptr, nullptr, &errmsg) != SQLITE_OK) {
    set_error(error, handle_, "cannot create critiques index");
    sqlite3_free(errmsg);
    return false;
  }
  sqlite3_free(errmsg);

  // schema_version: stamp on a fresh database; refuse an unknown newer one.
  {
    std::optional<Statement> maybe =
        prepare("SELECT version FROM schema_version LIMIT 1;", error);
    if (!maybe) return false;
    Statement version = std::move(*maybe);
    std::string row_error;
    if (version.step_row(&row_error)) {
      const int existing = version.integer_int(0);
      if (existing > kSchemaVersion) {
        set_error(error, handle_,
                  "database schema is newer than this build supports");
        return false;
      }
    } else if (!row_error.empty()) {
      set_error(error, handle_, "cannot read schema version");
      return false;
    }
  }
  // Migration: reviews.atomic arrived with the W5 gate calibration; add
  // it when an older database lacks the column.  It runs after the
  // newer-schema refusal so an unknown-newer database is never mutated.
  {
    bool has_atomic = false;
    {
      std::optional<Statement> check = prepare(
          "SELECT 1 FROM pragma_table_info('reviews') WHERE name = 'atomic';",
          error);
      if (check) {
        std::string step_error;
        has_atomic = check->step_row(&step_error);
      }
    }
    if (!has_atomic) {
      char* errmsg = nullptr;
      if (sqlite3_exec(handle_,
                       "ALTER TABLE reviews ADD COLUMN atomic INT NOT NULL "
                       "DEFAULT 0;",
                       nullptr, nullptr, &errmsg) != SQLITE_OK) {
        set_error(error, handle_, "cannot migrate reviews.atomic");
        sqlite3_free(errmsg);
        return false;
      }
    }
  }
  {
    std::optional<Statement> maybe = prepare(
        "INSERT INTO schema_version(version) SELECT ? WHERE NOT EXISTS "
        "(SELECT 1 FROM schema_version);",
        error);
    if (!maybe) return false;
    Statement stamp = std::move(*maybe);
    std::string bind_error;
    if (!stamp.bind(1, static_cast<std::int64_t>(kSchemaVersion), &bind_error)) {
      set_error(error, handle_, "cannot stamp schema version");
      return false;
    }
    if (stamp.step(&bind_error) || !bind_error.empty()) {
      set_error(error, handle_, "cannot stamp schema version");
      return false;
    }
  }
  return true;
}

bool Db::exec(std::string_view sql, std::string* error) {
  char* errmsg = nullptr;
  if (sqlite3_exec(handle_, std::string(sql).c_str(), nullptr, nullptr,
                   &errmsg) != SQLITE_OK) {
    set_error(error, handle_, "exec failed");
    sqlite3_free(errmsg);
    return false;
  }
  sqlite3_free(errmsg);
  return true;
}

std::optional<Statement> Db::prepare(std::string_view sql, std::string* error) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(handle_, std::string(sql).c_str(), -1, &stmt, nullptr) !=
      SQLITE_OK) {
    set_error(error, handle_, "prepare failed");
    return std::nullopt;
  }
  return Statement(stmt);
}

namespace {

Node read_node(Statement& stmt) {
  Node node;
  node.id = stmt.integer(0);
  node.code = stmt.text(1);
  node.path = stmt.text(2);
  node.parent_id = stmt.integer(3);
  node.layer = stmt.integer_int(4);
  node.title = stmt.text(5);
  node.origin = stmt.text(6);
  node.hlc = static_cast<std::uint64_t>(stmt.integer(7));
  node.tombstone = stmt.integer_int(8);
  return node;
}

constexpr const char* kNodeColumns =
    "id, code, path, parent_id, layer, title, origin, hlc, tombstone";

}  // namespace

std::optional<Node> Db::node_by_path(std::string_view path, std::string* error) const {
  std::optional<Statement> stmt = prepare(
      std::string("SELECT ") + kNodeColumns + " FROM nodes WHERE path = ?1;",
      error);
  if (!stmt) return std::nullopt;
  std::string step_error;
  if (!stmt->bind(1, path, &step_error) || !stmt->step(&step_error)) {
    if (!step_error.empty()) set_error(error, handle_, "node lookup failed");
    return std::nullopt;
  }
  Node node = read_node(*stmt);
  return node;
}

std::vector<Node> Db::nodes_in_subtree(std::string_view path, std::string* error) const {
  std::optional<Statement> stmt = prepare(
      std::string("SELECT ") + kNodeColumns +
          " FROM nodes WHERE path = ?1 OR (path >= ?2 AND path < ?3) "
          "ORDER BY path;",
      error);
  if (!stmt) return {};
  const std::string bound_path(path);
  const std::string floor = bound_path + "-";
  const std::string ceiling = bound_path + ".";
  std::string step_error;
  if (!stmt->bind(1, bound_path, &step_error) ||
      !stmt->bind(2, floor, &step_error) ||
      !stmt->bind(3, ceiling, &step_error)) {
    set_error(error, handle_, "subtree bind failed");
    return {};
  }
  std::vector<Node> nodes;
  while (stmt->step_row(&step_error)) nodes.push_back(read_node(*stmt));
  if (!step_error.empty()) set_error(error, handle_, "subtree query failed");
  return nodes;
}

std::vector<Node> Db::nodes_at_layer(int layer, std::string* error) const {
  std::optional<Statement> stmt = prepare(
      std::string("SELECT ") + kNodeColumns +
          " FROM nodes WHERE layer = ?1 AND tombstone = 0 ORDER BY path;",
      error);
  if (!stmt) return {};
  std::string step_error;
  if (!stmt->bind(1, static_cast<std::int64_t>(layer), &step_error)) {
    set_error(error, handle_, "layer bind failed");
    return {};
  }
  std::vector<Node> nodes;
  while (stmt->step_row(&step_error)) nodes.push_back(read_node(*stmt));
  if (!step_error.empty()) set_error(error, handle_, "layer query failed");
  return nodes;
}

std::vector<Node> Db::all_nodes(std::string* error) const {
  std::optional<Statement> stmt = prepare(
      std::string("SELECT ") + kNodeColumns +
          " FROM nodes WHERE tombstone = 0 ORDER BY path;",
      error);
  if (!stmt) return {};
  std::string step_error;
  std::vector<Node> nodes;
  while (stmt->step_row(&step_error)) nodes.push_back(read_node(*stmt));
  if (!step_error.empty()) set_error(error, handle_, "all-nodes query failed");
  return nodes;
}

std::vector<Node> Db::nodes_with_path_prefix(std::string_view prefix,
                                             std::string* error) const {
  std::optional<Statement> stmt = prepare(
      std::string("SELECT ") + kNodeColumns +
          " FROM nodes WHERE path >= ?1 AND path < ?2 ORDER BY path;",
      error);
  if (!stmt) return {};
  const std::string bound_prefix(prefix);
  const std::string ceiling = bound_prefix + ".";
  std::string step_error;
  if (!stmt->bind(1, bound_prefix, &step_error) ||
      !stmt->bind(2, ceiling, &step_error)) {
    set_error(error, handle_, "prefix bind failed");
    return {};
  }
  std::vector<Node> nodes;
  while (stmt->step_row(&step_error)) nodes.push_back(read_node(*stmt));
  if (!step_error.empty()) set_error(error, handle_, "prefix query failed");
  return nodes;
}

std::vector<Review> Db::open_reviews(std::string* error) const {
  std::optional<Statement> stmt = prepare(
      "SELECT path, reason, state, note, atomic, hlc, origin FROM reviews "
      "ORDER BY path;",
      error);
  if (!stmt) return {};
  std::string step_error;
  std::vector<Review> reviews;
  while (stmt->step_row(&step_error)) {
    Review review;
    review.path = stmt->text(0);
    review.reason = stmt->text(1);
    review.state = stmt->text(2);
    review.note = stmt->text(3);
    review.atomic = stmt->integer_int(4);
    review.hlc = static_cast<std::uint64_t>(stmt->integer(5));
    review.origin = stmt->text(6);
    reviews.push_back(std::move(review));
  }
  if (!step_error.empty()) set_error(error, handle_, "review query failed");
  return reviews;
}

std::vector<Campaign> Db::campaigns(std::string* error) const {
  std::optional<Statement> stmt = prepare(
      "SELECT id, spec, state, hlc, origin FROM campaigns ORDER BY id;", error);
  if (!stmt) return {};
  std::string step_error;
  std::vector<Campaign> campaigns;
  while (stmt->step_row(&step_error)) {
    Campaign campaign;
    campaign.id = stmt->text(0);
    campaign.spec = stmt->text(1);
    campaign.state = stmt->text(2);
    campaign.hlc = static_cast<std::uint64_t>(stmt->integer(3));
    campaign.origin = stmt->text(4);
    campaigns.push_back(std::move(campaign));
  }
  if (!step_error.empty()) set_error(error, handle_, "campaign query failed");
  return campaigns;
}

std::vector<Settle> Db::settles(std::string* error) const {
  std::optional<Statement> stmt = prepare(
      "SELECT path, kept, atomic, attempts, score, campaign, hlc, origin "
      "FROM settles ORDER BY path;",
      error);
  if (!stmt) return {};
  std::string step_error;
  std::vector<Settle> settles;
  while (stmt->step_row(&step_error)) {
    Settle settle;
    settle.path = stmt->text(0);
    settle.kept = stmt->integer_int(1);
    settle.atomic = stmt->integer_int(2);
    settle.attempts = stmt->integer_int(3);
    settle.score = stmt->text(4);
    settle.campaign = stmt->text(5);
    settle.hlc = static_cast<std::uint64_t>(stmt->integer(6));
    settle.origin = stmt->text(7);
    settles.push_back(std::move(settle));
  }
  if (!step_error.empty()) set_error(error, handle_, "settle query failed");
  return settles;
}

std::int64_t Db::max_node_id(std::string* error) const {
  std::optional<Statement> stmt =
      prepare("SELECT COALESCE(MAX(id), 0) FROM nodes;", error);
  if (!stmt) return 0;
  std::string step_error;
  if (!stmt->step_row(&step_error)) return 0;
  return stmt->integer(0);
}

std::int64_t Db::max_tk_value(std::string* error) const {
  std::optional<Statement> stmt = prepare(
      "SELECT COALESCE(MAX(CAST(substr(code, 3) AS INTEGER)), 0) FROM nodes "
      "WHERE code LIKE 'TK%';",
      error);
  if (!stmt) return 0;
  std::string step_error;
  if (!stmt->step_row(&step_error)) return 0;
  return stmt->integer(0);
}

std::string Db::spend_usd(std::string* error) const {
  std::optional<Statement> stmt = prepare(
      "SELECT COALESCE(SUM(CAST(cost AS REAL)), 0.0) FROM attempts;", error);
  if (!stmt) return "0";
  std::string step_error;
  if (!stmt->step_row(&step_error)) return "0";
  const double value = sqlite3_column_double(stmt->handle(), 0);
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.10f", value);
  std::string text(buffer);
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  return text.empty() ? "0" : text;
}

bool Db::export_csv(const std::filesystem::path& dir, int max_layer,
                    std::string* error) const {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    set_error(error, nullptr, "cannot create export directory");
    return false;
  }
  for (int layer = 0; layer <= max_layer && layer <= 9; ++layer) {
    std::vector<Node> nodes = nodes_at_layer(layer, error);
    if (!error->empty()) return false;
    char name[32];
    std::snprintf(name, sizeof(name), "Onion - level_%02d.csv", layer);
    std::ofstream out(dir / name, std::ios::binary);
    if (!out) {
      set_error(error, nullptr, "cannot write export file");
      return false;
    }
    if (layer == 0) {
      out << "code,title\n";
      for (const Node& node : nodes) {
        out << node.code << "," << csv_field(node.title) << "\n";
      }
    } else {
      out << "code,path,title\n";
      for (const Node& node : nodes) {
        out << node.code << "," << csv_field(node.path) << ","
            << csv_field(node.title) << "\n";
      }
    }
  }
  return true;
}

std::vector<Attempt> Db::attempts(std::string* error) const {
  std::optional<Statement> stmt = prepare(
      "SELECT path, n, score, passed, tokens_in, tokens_cached, tokens_out, "
      "cost, campaign, hlc, origin FROM attempts ORDER BY origin, hlc;",
      error);
  if (!stmt) return {};
  std::string step_error;
  std::vector<Attempt> attempts;
  while (stmt->step_row(&step_error)) {
    Attempt attempt;
    attempt.path = stmt->text(0);
    attempt.n = stmt->integer_int(1);
    attempt.score = stmt->text(2);
    attempt.passed = stmt->integer_int(3);
    attempt.tokens_in = stmt->integer(4);
    attempt.tokens_cached = stmt->integer(5);
    attempt.tokens_out = stmt->integer(6);
    attempt.cost = stmt->text(7);
    attempt.campaign = stmt->text(8);
    attempt.hlc = static_cast<std::uint64_t>(stmt->integer(9));
    attempt.origin = stmt->text(10);
    attempts.push_back(std::move(attempt));
  }
  if (!step_error.empty()) set_error(error, handle_, "attempts query failed");
  return attempts;
}

}  // namespace onion::store
