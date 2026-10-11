#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <map>
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

// A verified batch pulled from a peer (M6): appended to the local copy of
// that origin's log, durably, then folded with the batch's own origin.
struct AppendForeign {
  log::Batch batch;
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
  std::int64_t hlc{};        // latest (hlc, origin) wins
  std::string origin;
};

class StateWriter {
 public:
  struct Options {
    std::filesystem::path data_dir;   // contains onion.db and oplog/
    std::string origin{"dev0000000000000001"};
    std::unique_ptr<log::Signer> signer;
    // When set (M4+), batch 1 chains from the public-key hash and the
    // replay verifies every batch against this key; a torn or tampered
    // tail is cut. When null, the NullSigner behaviour of M3 applies.
    std::shared_ptr<const log::DeviceKey> device_key;
    // Segment seal threshold (tests use small values to force rotation).
    std::uint64_t seal_bytes{64u * 1024u * 1024u};
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
  void submit(AppendForeign command);
  // Blocks until every queued command is durable and folded.
  void sync();
  // Writes one checkpoint (VACUUM INTO + signed manifest), keeps the last 3.
  bool write_checkpoint(std::string* error);
  // Drains and joins the writer thread.
  void stop();

  // The applied vector (origin -> last record seq).
  std::map<std::string, std::int64_t> vector() const;
  // The head hash of an origin's local chain (empty when nothing is known;
  // batch 1 then chains from the genesis public-key hash).
  std::optional<std::string> origin_prev_hash(std::string_view origin) const;
  // Read side (any thread; WAL reads plus a shared index).
  const store::Db& db() const;
  log::Segments& segments();
  bool settled(std::string_view path) const;
  bool leaf(std::string_view path) const;         // settled, cannot split
  bool needs_review(std::string_view path) const;
  std::optional<CampaignInfo> campaign(std::string_view id) const;
  // Fleet-wide RESERVE totals per campaign, folded in (hlc, origin) order
  // and CAPPED at the campaign's loop budget (decision 0014): entries past
  // the budget are void, so overshoot is at most one chunk per device.
  std::int64_t reserved_total(std::string_view campaign) const;
  // Whether one specific RESERVE record (origin, hlc) falls past the cap:
  // its device must stop starting new attempts for the campaign.
  bool reservation_void(std::string_view campaign, std::string_view origin,
                        std::int64_t hlc) const;
  // Per-origin RESERVE totals for one campaign, for diagnostics.
  std::map<std::string, std::int64_t> reserved_by_origin(
      std::string_view campaign) const;
  std::vector<CampaignInfo> campaigns() const;
  std::uint64_t next_seq() const;
  std::uint64_t next_batch() const;

 private:
  struct Command {
    enum class Kind { Attempt, Terminal, Import, Foreign, Sync, Shutdown } kind;
    log::Record attempt;
    std::vector<log::Record> records;
    std::vector<DevImportedNode> nodes;
    log::Batch foreign;
  };

  void run();
  bool flush_pending_attempts(std::string* error);
  bool append_batch(std::vector<log::Record> records, std::string* error);
  void update_applied(std::string_view origin,
                      const std::vector<log::Record>& records);
  std::map<std::string, std::unique_ptr<log::Segments>> foreign_segments_;
  // Per-origin chain heads (own origin included) for sync continuation.
  mutable std::shared_mutex chain_mutex_;
  std::map<std::string, std::string> origin_prev_hashes_;
  void fold(const log::Record& record, std::string_view batch_origin,
            std::string* error);
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
  std::chrono::steady_clock::time_point last_checkpoint_;
  std::atomic<std::uint64_t> applied_since_checkpoint_{0};

  // Shared read index, rebuilt by replay and kept current by the fold.
  mutable std::shared_mutex index_mutex_;
  std::unordered_set<std::string> settled_paths_;
  std::unordered_set<std::string> leaf_paths_;    // settled with end != children
  std::unordered_set<std::string> review_paths_;  // needs_review, state open
  std::vector<CampaignInfo> campaigns_;
  std::map<std::string, std::int64_t> reserved_by_campaign_;
  std::map<std::string, std::int64_t> reserved_by_origin_campaign_;  // origin|campaign
  std::unordered_set<std::string> seen_reserve_keys_;  // origin:hlc

  // Decision 0014: the per-campaign RESERVE entry list for the capped,
  // order-independent fold.
  struct ReserveEntry {
    std::int64_t hlc{};
    std::string origin;
    std::int64_t attempts{};
  };
  std::map<std::string, std::vector<ReserveEntry>> reserve_entries_;
  std::map<std::string, std::int64_t> campaign_budgets_;  // from CAMPAIGN spec
  std::int64_t campaign_budget(std::string_view campaign) const;
  std::vector<ReserveEntry> reserve_entries_sorted(
      std::string_view campaign) const;  // sorted by (hlc, origin)
};

}  // namespace onion::state
