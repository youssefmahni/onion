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

// M8 pre-work: the push handler learns the pusher's sync address without
// touching SQLite itself; the writer thread owns the members-table write.
struct SetMemberSyncUrl {
  std::string device_id;
  std::string sync_url;
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
  log::Hlc hlc{};      // latest (hlc, origin) wins
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
    // The offline admin key's public key (hex), learned at genesis. When
    // set, a MEMBER_EVICT record signed by it is valid without a
    // majority (DESIGN › Eviction and retirement).
    std::string admin_public_key_hex;
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
  void submit(SetMemberSyncUrl command);
  // Blocks until every queued command is durable and folded.
  void sync();
  // Writes one checkpoint (VACUUM INTO + signed manifest), keeps the last 3.
  bool write_checkpoint(std::string* error);
  // The data directory (checkpoints live next to onion.db).
  const std::filesystem::path& data_dir() const { return options_.data_dir; }
  // Drains and joins the writer thread.
  void stop();

  // The applied vector (origin -> last record seq).
  std::map<std::string, std::int64_t> vector() const;
  // The head hash of an origin's local chain (empty when nothing is known;
  // batch 1 then chains from the genesis public-key hash).
  std::optional<std::string> origin_prev_hash(std::string_view origin) const;
  // Read side (any thread; WAL reads plus a shared index).
  const store::Db& db() const;

  // Per-batch hashes, recorded by the writer thread for every batch it
  // appends or replays: the authoritative "what is stored for exactly
  // (origin, number)" answer behind the push endpoint's duplicate /
  // forked-log / gap classification. Empty when the number was never
  // stored; the table is rebuilt by replay on every start.
  std::optional<std::string> stored_batch_hash(std::string_view origin,
                                               std::uint64_t number) const;
  std::uint64_t latest_batch_number(std::string_view origin) const;
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
                        log::Hlc hlc) const;
  // Per-origin RESERVE totals for one campaign, for diagnostics.
  std::map<std::string, std::int64_t> reserved_by_origin(
      std::string_view campaign) const;
  // M7 joining: whether an invitation nonce was already used (single-use
  // tokens — the MEMBER_ADD fold records each nonce).
  bool join_nonce_seen(std::string_view nonce) const;
  // M7 joining: whether the slot already belongs to another device.
  bool member_slot_collides(std::int64_t slot,
                            std::string_view device_id) const;
  // M7 joining: whether the members table holds this device.
  bool member_exists(std::string_view device_id) const;
  // M7 joining: a member's public key (32 bytes) from the members table.
  std::optional<std::vector<std::uint8_t>> member_public_key(
      std::string_view device_id) const;
  // True when the member row exists and is neither EVICTED nor LEFT
  // (those states are terminal). Address recording and other
  // current-member-only decisions use this, not member_public_key,
  // because an evicted member's key stays in the table.
  bool member_is_current(std::string_view device_id) const;
  // M7 stealing: whether another device holds an unexpired CLAIM on the
  // path (timed from THIS device's receipt — never the claimer's clock).
  // Returns the claimer's device id, or empty.
  std::string claim_holder(std::string_view path, std::string_view my_device,
                           std::int64_t now_ms) const;
  // M7 re-expansion lock: whether any ancestor of the path has an open
  // REEXPAND (its subtree pauses until the re-expansion settles).
  bool reexpand_open(std::string_view path) const;
  // M7 joining: every member row (device_id, pubkey hex, slot, state).
  struct MemberRow {
    std::string device_id;
    std::string pubkey_hex;
    std::int64_t slot{};
    std::string state;
    std::string sync_url;
    std::string swim_url;
  };
  // The same lookups on a caller-supplied connection (the sync server's
  // per-handler read-only connections — never the writer's handle).
  static std::optional<std::string> stored_batch_hash_on(
      const store::Db& db, std::string_view origin, std::uint64_t number);
  static std::uint64_t latest_batch_number_on(const store::Db& db,
                                              std::string_view origin);
  static std::map<std::string, std::int64_t> vector_on(const store::Db& db);
  static std::vector<MemberRow> members_table_on(const store::Db& db);
  static std::optional<std::vector<std::uint8_t>> member_public_key_on(
      const store::Db& db, std::string_view device_id);
  static bool member_is_current_on(const store::Db& db,
                                   std::string_view device_id);
  static bool member_exists_on(const store::Db& db,
                               std::string_view device_id);
  static bool member_slot_collides_on(const store::Db& db, std::int64_t slot,
                                      std::string_view device_id);

  // A per-thread read-only SQLite connection for server handler threads
  // (WAL readers never block behind the writer's transactions). Falls
  // back to the shared handle only when the read-only open fails.
  const store::Db& read_db() const;

  // A forked log (the same (origin, number) carried by two different
  // hashes) is recorded for /api/state and never applied.
  struct ForkEvent {
    std::string origin;
    std::uint64_t number{};
    std::string incoming_hash;
    std::string stored_hash;
  };
  void note_fork(ForkEvent fork);
  std::vector<ForkEvent> forks() const;

  // M7: a member's sync (TCP) address from the durable table, if known.
  std::vector<MemberRow> members_table() const;
  // M7: a member's sync (TCP) address from the durable table, if known.
  std::optional<std::string> member_sync_url(std::string_view device_id) const;
  std::optional<std::string> member_swim_url(std::string_view device_id) const;
  std::vector<CampaignInfo> campaigns() const;
  std::uint64_t next_seq() const;
  std::uint64_t next_batch() const;
  // Duplicate-path rows skipped by the last development import (INSERT OR
  // IGNORE keeps the first row); reported by the CLI.
  std::size_t last_import_skipped() const { return import_skipped_.load(); }

 private:
  struct Command {
    enum class Kind {
      Attempt,
      Terminal,
      Import,
      Foreign,
      MemberUrl,
      Sync,
      Shutdown
    } kind;
    log::Record attempt;
    std::vector<log::Record> records;
    std::vector<DevImportedNode> nodes;
    log::Batch foreign;
    SetMemberSyncUrl member_url;
  };

  void run();
  bool flush_pending_attempts(std::string* error);
  // Appends and folds one batch.  The crash hook fires at hook_step_base
  // (batch durable, store stale) and hook_step_base + 1 (fold done).
  // Terminal batches use the default base 1; the group-commit attempt flush
  // uses base 3, so the fork tests can kill at every commit-order step (C4).
  bool append_batch(std::vector<log::Record> records, std::string* error,
                    int hook_step_base = 1);
  void update_applied(std::string_view origin,
                      const std::vector<log::Record>& records);
  void record_batch(std::string_view origin, std::uint64_t number,
                    std::string_view hash);
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
  std::atomic<std::size_t> import_skipped_{0};
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
    log::Hlc hlc{};
    std::string origin;
    std::int64_t attempts{};
  };
  std::unordered_set<std::string> seen_join_nonces_;  // single-use tokens
  // M7 stealing: per-path claims (claimer, receipt ms, lease s) and the
  // re-expansion lock set. Both are replay-owned (no store table).
  struct ClaimEntry {
    std::string claimer;
    log::Hlc hlc{};           // the highest (hlc, origin) claim wins
    std::string origin;
    std::int64_t received_ms{};
    std::int64_t lease_s{600};
  };
  std::map<std::string, ClaimEntry> claims_;
  std::unordered_set<std::string> reexpand_paths_;
  std::map<std::string, std::vector<ReserveEntry>> reserve_entries_;
  std::map<std::string, std::int64_t> campaign_budgets_;  // from CAMPAIGN spec
  std::int64_t campaign_budget(std::string_view campaign) const;
  std::vector<ReserveEntry> reserve_entries_sorted(
      std::string_view campaign) const;  // sorted by (hlc, origin)

  // Forked-log sightings (M8 pre-work): recorded by the push handler or
  // the writer thread, surfaced by /api/state. Bounded to the latest 64.
  mutable std::mutex forks_mutex_;
  std::deque<ForkEvent> forks_;
};

}  // namespace onion::state
