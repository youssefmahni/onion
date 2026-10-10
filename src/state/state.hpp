#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <thread>
#include <vector>

#include "log/log.hpp"
#include "store/store.hpp"

namespace onion::state {
inline constexpr std::string_view component_name{"state"};

// The single state-writer thread. Every mutation of the log and of SQLite
// happens on this thread, in submission order, so the commit order from
// DESIGN.md › Joining is structural: records become durable before the
// store is updated.

struct AppendAttempt {
  log::Record attempt;
};

// Terminal batch for one node: children ADD_NODE records, CRITIQUE, ATOMIC,
// and SETTLE last — or a single NEEDS_REVIEW record.
struct CommitTerminal {
  std::vector<log::Record> records;
};

// Unsigned development import (M3 only; M4 replaces it with the genesis
// import). Nodes go straight into SQLite, never into the log.
struct DevImportedNode {
  std::string code;
  std::string path;
  int layer{};
  std::string title;
};

struct DevImport {
  std::vector<DevImportedNode> nodes;
};

struct CampaignInfo {
  std::string id;
  std::string spec;    // JSON
  std::string state;   // running | paused | stopped
};

class StateWriter {
 public:
  struct Options {
    std::filesystem::path data_dir;   // contains onion.db and oplog/
    std::string origin{"dev0000000000000001"};
    std::unique_ptr<log::Signer> signer;
    // Test-only crash injection, called at the commit steps:
    // 1 = terminal batch durable, before the fold; 2 = after the fold.
    // Throwing (or terminating) from it simulates a power cut.
    std::function<void(int)> crash_hook;
  };

  explicit StateWriter(Options options);
  ~StateWriter();
  StateWriter(const StateWriter&) = delete;
  StateWriter& operator=(const StateWriter&) = delete;

  // Opens the store and segments, replays the log, starts the thread.
  bool start(std::string* error);
  void submit(AppendAttempt command);
  void submit(CommitTerminal command);
  void submit(DevImport command);
  // Blocks until every queued command is durable and folded.
  void sync();
  // Drains and joins the writer thread.
  void stop();

  // Read side (any thread; WAL reads plus a shared index).
  const store::Db& db() const;
  log::Segments& segments();
  bool settled(std::string_view path) const;
  bool leaf(std::string_view path) const;         // settled, cannot split
  bool needs_review(std::string_view path) const;
  std::optional<CampaignInfo> campaign(std::string_view id) const;
  std::vector<CampaignInfo> campaigns() const;
  std::uint64_t next_seq() const;
  std::uint64_t next_batch() const;

 private:
  struct Command {
    enum class Kind { Attempt, Terminal, Import, Sync, Shutdown } kind;
    log::Record attempt;
    std::vector<log::Record> records;
    std::vector<DevImportedNode> nodes;
  };

  void run();
  bool flush_pending_attempts(std::string* error);
  bool append_batch(std::vector<log::Record> records, std::string* error);
  void fold(const log::Record& record, std::string* error);
  void rebuild_index();
  void replay(std::string* error);

  Options options_;
  store::Db db_;
  std::unique_ptr<log::Segments> segments_;
  log::HlcClock hlc_;
  std::unique_ptr<log::Signer> signer_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Command> queue_;
  bool stopping_{false};
  bool started_{false};
  std::thread thread_;
  std::atomic<bool> synced_flag_{false};
  std::condition_variable synced_cv_;

  std::atomic<std::uint64_t> next_seq_{1};
  std::atomic<std::uint64_t> next_batch_{1};
  std::string prev_hash_;  // empty until the first batch

  // Writer-thread state: pending ATTEMPT group-commit.
  std::vector<log::Record> pending_attempts_;
  std::chrono::steady_clock::time_point last_flush_;

  // Shared read index, rebuilt by replay and kept current by the fold.
  mutable std::shared_mutex index_mutex_;
  std::unordered_set<std::string> settled_paths_;
  std::unordered_set<std::string> leaf_paths_;    // settled with end != children
  std::unordered_set<std::string> review_paths_;  // needs_review, state open
  std::vector<CampaignInfo> campaigns_;
};

}  // namespace onion::state
