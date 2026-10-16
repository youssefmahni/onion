#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onion::store {
class Db;
}

namespace onion::viewcache {
inline constexpr std::string_view component_name{"viewcache"};

// The dashboard's sidecar cache (plan §6.2): `viewcache.db` beside
// `onion.db`. It holds per-path child/subtree counts, an FTS5 index over
// titles, and the applied vector it was built from. It is a pure cache:
// deleting the file is always safe. Reads happen on the cache's own
// connection, so dashboard queries never touch the node's writer.
//
// Threading contract: Cache itself is synchronous. The node wires it to a
// background thread (full rebuild on a vector/version mismatch, throttled;
// incremental updates per folded record); `view --db` builds it once up
// front. The caller owns the source's read-only connection.
class Cache {
 public:
  struct Options {
    std::filesystem::path sidecar_path;  // <data_dir>/viewcache.db
    std::filesystem::path source_path;   // the read-only onion.db handle
  };

  struct Hit {
    std::string path;
    double rank{};
  };

  struct Counts {
    std::int64_t children{};
    std::int64_t subtree{};
  };

  explicit Cache(Options options);
  ~Cache();
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  bool start(std::string* error);
  void stop();

  // Full rebuild from the folded source (a read-only onion.db handle):
  // counts, FTS and meta. Transactional; leaves the previous content
  // intact on failure.
  bool rebuild(const store::Db& source, std::string* error);
  // True when the cache's recorded applied vector or its schema version
  // differ from the source — the caller rebuilds.
  bool stale(const store::Db& source, bool* is_stale, std::string* error);

  // Incremental updates (fold notifications): the caller feeds every
  // folded record; non-node records are no-ops.
  bool node_added(std::string_view path, std::string_view title,
                  std::string* error);
  bool node_removed(std::string_view path, std::string* error);

  std::optional<Counts> counts_for(std::string_view path,
                                   std::string* error) const;
  std::vector<Hit> search(std::string_view query, int limit,
                          std::string* error) const;
  // The FTS5 query language: callers append "*" to the last token for
  // prefix matching.
  bool ready() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Serializes access: dashboard handler threads read while the node's
  // maintenance thread rebuilds or applies incremental updates.
  mutable std::mutex mutex_;
};

}  // namespace onion::viewcache
