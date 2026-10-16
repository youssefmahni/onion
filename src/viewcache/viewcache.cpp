#include "viewcache.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <unordered_map>

#include <sqlite3.h>

#include "store/store.hpp"

namespace onion::viewcache {

inline constexpr int kCacheVersion = 1;

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS counts(path TEXT PRIMARY KEY,
                                  children INTEGER NOT NULL,
                                  subtree INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS fts_titles(rowid INTEGER PRIMARY KEY,
                                    title TEXT,
                                    path TEXT);
CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(
    title, path, content='fts_titles', content_rowid='rowid');
CREATE TABLE IF NOT EXISTS meta(version INTEGER NOT NULL,
                                applied_vector TEXT NOT NULL);
)sql";

std::string vector_json(const store::Db& source, std::string* error) {
  std::string out = "{";
  bool first = true;
  auto statement = source.prepare(
      "SELECT origin, seq FROM applied ORDER BY origin;", error);
  if (!statement) return out + "}";
  std::string step_error;
  while (statement->step_row(&step_error)) {
    if (!first) out += ",";
    first = false;
    out += "\"" + statement->text(0) + "\":" +
           std::to_string(statement->integer(1));
  }
  out += "}";
  return out;
}

}  // namespace

struct Cache::Impl {
  Options options;
  store::Db db{options.sidecar_path};
  std::unique_ptr<store::Db> source;  // read-only onion.db handle
  bool ready{false};
};

Cache::Cache(Options options)
    : impl_(new Impl{std::move(options)}) {}
Cache::~Cache() { stop(); }

bool Cache::start(std::string* error) {
  std::lock_guard lock(mutex_);
  if (!impl_->db.open(error)) return false;
  if (!impl_->db.exec(kSchema, error)) return false;
  impl_->source =
      std::make_unique<store::Db>(impl_->options.source_path);
  if (!impl_->source->open_readonly(error)) {
    // A missing source is fine before the first rebuild (view --db may
    // build before the copy exists); the rebuild reports the real error.
    impl_->source.reset();
    return true;
  }
  return true;
}

bool Cache::ready() const {
  std::lock_guard lock(mutex_);
  return impl_->ready;
}

void Cache::stop() {
  std::lock_guard lock(mutex_);
  if (impl_->source) impl_->source->close();
  impl_->db.close();
}

bool Cache::rebuild(const store::Db& source, std::string* error) {
  std::lock_guard lock(mutex_);
  // Rebuild inside one transaction; on any failure the cache keeps its
  // previous content (or an empty one on first build).
  impl_->db.exec("BEGIN;", error);
  const auto rollback = [&]() { impl_->db.exec("ROLLBACK;", nullptr); };

  if (!impl_->db.exec(
          "DELETE FROM counts; DELETE FROM fts_titles; "
          "INSERT INTO fts(fts) VALUES('delete');",
          error)) {
    rollback();
    return false;
  }

  // Counts: children via one group-by; subtree via a bottom-up fold over
  // the parent links (paths are prefix-encoded, so longer paths process
  // after their parents).
  struct Row {
    std::int64_t id;
    std::string path;
    std::int64_t parent_id;
    std::string title;
  };
  std::vector<Row> rows;
  for (const auto& node : source.all_nodes(error)) {
    if (node.tombstone) continue;
    rows.push_back(Row{node.id, node.path, node.parent_id, node.title});
  }
  if (error && !error->empty()) {
    rollback();
    return false;
  }

  std::unordered_map<std::int64_t, std::vector<std::int64_t>> children;
  std::unordered_map<std::int64_t, std::int64_t> subtree;
  for (const auto& row : rows) {
    subtree.emplace(row.id, 0);
    if (row.parent_id > 0) children[row.parent_id].push_back(row.id);
  }
  // Longest paths first: every child is strictly longer than its parent,
  // so a single pass accumulates child subtrees before parents read them.
  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.path.size() > b.path.size();
  });
  for (const auto& row : rows) {
    std::int64_t total = 0;
    const auto found = children.find(row.id);
    if (found != children.end())
      for (const std::int64_t child : found->second)
        total += 1 + subtree[child];
    subtree[row.id] = total;
  }

  auto insert_counts = impl_->db.prepare(
      "INSERT INTO counts(path, children, subtree) VALUES (?1, ?2, ?3);",
      error);
  auto insert_fts = impl_->db.prepare(
      "INSERT INTO fts_titles(rowid, title, path) VALUES (?1, ?2, ?3);",
      error);
  if (!insert_counts || !insert_fts) {
    rollback();
    return false;
  }
  std::string bind_error;
  for (const auto& row : rows) {
    const std::int64_t kids =
        children.count(row.id) ? static_cast<std::int64_t>(
                                     children.at(row.id).size())
                               : 0;
    if (!insert_counts->bind(1, row.path, &bind_error) ||
        !insert_counts->bind(2, kids, &bind_error) ||
        !insert_counts->bind(3, subtree.at(row.id), &bind_error)) {
      rollback();
      if (error) *error = "counts bind failed: " + bind_error;
      return false;
    }
    insert_counts->step(&bind_error);
    if (!bind_error.empty()) {
      rollback();
      if (error) *error = "counts insert failed: " + bind_error;
      return false;
    }
    insert_counts->reset(&bind_error);
    if (!insert_fts->bind(1, row.id, &bind_error) ||
        !insert_fts->bind(2, row.title, &bind_error) ||
        !insert_fts->bind(3, row.path, &bind_error)) {
      rollback();
      if (error) *error = "fts bind failed: " + bind_error;
      return false;
    }
    insert_fts->step(&bind_error);
    if (!bind_error.empty()) {
      rollback();
      if (error) *error = "fts insert failed: " + bind_error;
      return false;
    }
    insert_fts->reset(&bind_error);
  }

  impl_->db.exec("DELETE FROM meta;", error);
  auto write_meta = impl_->db.prepare(
      "INSERT INTO meta(version, applied_vector) VALUES (?1, ?2);", error);
  if (!write_meta) {
    rollback();
    return false;
  }
  const std::string vector = vector_json(source, error);
  if (!write_meta->bind(1, kCacheVersion, &bind_error) ||
      !write_meta->bind(2, vector, &bind_error)) {
    rollback();
    if (error) *error = "meta bind failed: " + bind_error;
    return false;
  }
  write_meta->step(&bind_error);
  if (!bind_error.empty()) {
    rollback();
    if (error) *error = "meta write failed: " + bind_error;
    return false;
  }

  // External-content FTS5 indexes nothing until told: one rebuild pass
  // over the freshly inserted content, still inside the transaction.
  impl_->db.exec("INSERT INTO fts(fts) VALUES('rebuild');", error);
  impl_->db.exec("COMMIT;", error);
  impl_->ready = error == nullptr || error->empty();
  return impl_->ready;
}

bool Cache::stale(const store::Db& source, bool* is_stale,
                  std::string* error) {
  std::lock_guard lock(mutex_);
  *is_stale = !impl_->ready;
  auto statement = impl_->db.prepare(
      "SELECT version, applied_vector FROM meta LIMIT 1;", error);
  if (!statement) return false;
  std::string step_error;
  std::string stored_vector;
  if (statement->step_row(&step_error)) {
    if (statement->integer_int(0) != kCacheVersion)
      *is_stale = true;
    else
      stored_vector = statement->text(1);
  } else {
    *is_stale = true;  // no meta row yet
  }
  const std::string current = vector_json(source, error);
  if (!*is_stale && stored_vector != current) *is_stale = true;
  return error == nullptr || error->empty();
}

namespace {

// Walks the ancestor chain of `node_id` and applies `delta` to every
// ancestor's subtree count (plus the direct parent's children count).
// The cache materializes ancestor rows first, so the deltas never drift.
bool adjust_counts(store::Db& cache_db, const store::Db& source,
                   std::int64_t node_id, std::int64_t delta,
                   std::string* error) {
  auto lookup = source.prepare(
      "SELECT path, parent_id FROM nodes WHERE id = ?1;", error);
  auto materialize = cache_db.prepare(
      "INSERT OR IGNORE INTO counts(path, children, subtree) VALUES "
      "(?1, 0, 0);",
      error);
  auto bump_subtree = cache_db.prepare(
      "UPDATE counts SET subtree = subtree + ?2 WHERE path = ?1;", error);
  auto bump_children = cache_db.prepare(
      "UPDATE counts SET children = children + ?2 WHERE path = ?1;", error);
  if (!lookup || !materialize || !bump_subtree || !bump_children)
    return false;

  std::string bind_error;
  std::int64_t current = node_id;
  bool direct_parent = true;
  while (current > 0) {
    lookup->reset(&bind_error);
    if (!lookup->bind(1, current, &bind_error)) return false;
    std::string step_error;
    if (!lookup->step_row(&step_error)) return true;  // chain ends
    const std::string path = lookup->text(0);
    const std::int64_t parent_id = lookup->integer(1);
    if (current == node_id) {  // the node itself is not its own descendant
      current = parent_id;
      continue;
    }
    materialize->reset(&bind_error);
    materialize->bind(1, path, &bind_error);
    materialize->step(&bind_error);
    bump_subtree->reset(&bind_error);
    if (!bump_subtree->bind(1, path, &bind_error) ||
        !bump_subtree->bind(2, delta, &bind_error))
      return false;
    bump_subtree->step(&bind_error);
    if (direct_parent) {
      bump_children->reset(&bind_error);
      if (!bump_children->bind(1, path, &bind_error) ||
          !bump_children->bind(2, delta, &bind_error))
        return false;
      bump_children->step(&bind_error);
      direct_parent = false;
    }
    current = parent_id;
  }
  return true;
}

}  // namespace

bool Cache::node_added(std::string_view path, std::string_view title,
                       std::string* error) {
  std::lock_guard lock(mutex_);
  if (!impl_->ready || !impl_->source) return true;
  std::string bind_error;
  auto lookup = impl_->source->prepare(
      "SELECT id, parent_id FROM nodes WHERE path = ?1;", error);
  if (!lookup) return false;
  lookup->bind(1, std::string(path), &bind_error);
  std::string step_error;
  std::int64_t node_id = 0;
  if (lookup->step_row(&step_error)) node_id = lookup->integer(0);
  if (node_id == 0) return true;

  if (!adjust_counts(impl_->db, *impl_->source, node_id, 1, error))
    return false;

  auto insert = impl_->db.prepare(
      "INSERT OR REPLACE INTO fts_titles(rowid, title, path) VALUES "
      "(?1, ?2, ?3);",
      error);
  auto index_row = impl_->db.prepare(
      "INSERT INTO fts(rowid, title, path) VALUES (?1, ?2, ?3);", error);
  if (!insert || !index_row) return false;
  if (!insert->bind(1, node_id, &bind_error) ||
      !insert->bind(2, std::string(title), &bind_error) ||
      !insert->bind(3, std::string(path), &bind_error))
    return false;
  insert->step(&bind_error);
  if (!bind_error.empty()) {
    if (error) *error = "fts content upsert failed: " + bind_error;
    return false;
  }
  if (!index_row->bind(1, node_id, &bind_error) ||
      !index_row->bind(2, std::string(title), &bind_error) ||
      !index_row->bind(3, std::string(path), &bind_error))
    return false;
  index_row->step(&bind_error);
  if (!bind_error.empty() && error)
    *error = "fts index upsert failed: " + bind_error;
  return error == nullptr || error->empty();
}

bool Cache::node_removed(std::string_view path, std::string* error) {
  std::lock_guard lock(mutex_);
  if (!impl_->ready || !impl_->source) return true;
  std::string bind_error;
  auto lookup = impl_->source->prepare(
      "SELECT id FROM nodes WHERE path = ?1;", error);
  if (!lookup) return false;
  lookup->bind(1, std::string(path), &bind_error);
  std::string step_error;
  std::int64_t node_id = 0;
  if (lookup->step_row(&step_error)) node_id = lookup->integer(0);
  if (node_id == 0) return true;

  if (!adjust_counts(impl_->db, *impl_->source, node_id, -1, error))
    return false;
  // The 'delete' special command needs the row's original column values.
  auto read_row = impl_->db.prepare(
      "SELECT title, path FROM fts_titles WHERE rowid = ?1;", error);
  auto remove = impl_->db.prepare(
      "DELETE FROM fts_titles WHERE rowid = ?1;", error);
  auto unindex = impl_->db.prepare(
      "INSERT INTO fts(fts, rowid, title, path) VALUES ('delete', ?1, ?2, "
      "?3);",
      error);
  if (!read_row || !remove || !unindex) return false;
  read_row->bind(1, node_id, &bind_error);
  std::string old_title, old_path;
  if (read_row->step_row(&step_error)) {
    old_title = read_row->text(0);
    old_path = read_row->text(1);
  }
  if (!old_title.empty() || !old_path.empty()) {
    if (!unindex->bind(1, node_id, &bind_error) ||
        !unindex->bind(2, old_title, &bind_error) ||
        !unindex->bind(3, old_path, &bind_error))
      return false;
    unindex->step(&bind_error);
  }
  remove->bind(1, node_id, &bind_error);
  remove->step(&bind_error);
  if (!bind_error.empty() && error)
    *error = "fts removal failed: " + bind_error;
  return error == nullptr || error->empty();
}

std::optional<Cache::Counts> Cache::counts_for(std::string_view path,
                                               std::string* error) const {
  std::lock_guard lock(mutex_);
  auto statement = impl_->db.prepare(
      "SELECT children, subtree FROM counts WHERE path = ?1;", error);
  if (!statement) return std::nullopt;
  std::string bind_error;
  statement->bind(1, std::string(path), &bind_error);
  std::string step_error;
  if (!statement->step_row(&step_error)) return std::nullopt;
  return Counts{statement->integer(0), statement->integer(1)};
}

std::vector<Cache::Hit> Cache::search(std::string_view query, int limit,
                                      std::string* error) const {
  std::lock_guard lock(mutex_);
  std::vector<Hit> hits;
  if (query.empty()) return hits;
  auto statement = impl_->db.prepare(
      "SELECT path, bm25(fts) FROM fts WHERE fts MATCH ?1 ORDER BY rank "
      "LIMIT ?2;",
      error);
  if (!statement) return hits;
  std::string bind_error;
  if (!statement->bind(1, std::string(query), &bind_error) ||
      !statement->bind(2, std::max(1, std::min(limit, 200)), &bind_error))
    return hits;
  std::string step_error;
  while (statement->step_row(&step_error))
    hits.push_back(Hit{statement->text(0), statement->integer(1) * 1.0});
  if (!step_error.empty() && error)
    *error = "fts query failed: " + step_error;
  return hits;
}

}  // namespace onion::viewcache
