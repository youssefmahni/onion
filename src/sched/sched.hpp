#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "gate/gate.hpp"
#include "llm/llm.hpp"
#include "state/state.hpp"
#include "store/store.hpp"

namespace onion::sched {
inline constexpr std::string_view component_name{"sched"};

// Campaign spec as written to the campaign file and folded into the
// CAMPAIGN record (decision 0008 covers the file format).
struct CampaignSpec {
  std::string id;
  std::vector<std::string> scope;         // path prefixes
  std::int64_t loop_budget{10000};
  std::string mode{"depth-first"};        // breadth-first | depth-first
  double gate{0.7};
  int max_attempts{3};
  double floor{0.5};
  std::int64_t chunk{50};                 // RESERVE chunk size
  int light_funnel_from{8};
  bool structure{false};                  // L1..L3 expansion allowed (M3: no)
  std::string model{"deepseek-flash"};
  std::string critic_model;               // empty = same as model
  bool auto_accept{false};                // M3: parsed, not acted on
};

std::optional<CampaignSpec> parse_campaign(const nlohmann::json& value,
                                           std::string* error);

// Node ids: TK + decimal of slot × 2^40 + counter (DESIGN › Device identity
// and ID slots). Legacy ids live in slot 0 and keep their numbers.
class NodeIdAllocator {
 public:
  // slot: the device's 20-bit slot; next: the highest counter already used
  // in this slot (from the store on start).
  NodeIdAllocator(std::int64_t slot, std::int64_t next_counter = 0);
  std::string allocate();             // next id, e.g. "TK00000000000000001"
  static std::int64_t counter_from_code(std::string_view code);  // 0 if not a TK id
  static std::int64_t id_value(std::int64_t slot, std::int64_t counter);

 private:
  std::int64_t slot_;
  std::int64_t counter_;
};

struct Ticket {
  std::string path;
  std::string title;
  int layer{};
  std::string campaign;
};

// Orders the open tickets of one campaign: shallowest first (breadth) or
// deepest first (depth), ties by path. A node is a ticket when it is inside
// the scope, not settled, not a leaf, has no open review of its own and no
// open review on any ancestor.
std::vector<Ticket> order_tickets(
    const std::vector<store::Node>& nodes,
    const std::function<bool(std::string_view)>& settled,
    const std::function<bool(std::string_view)>& leaf,
    const std::vector<std::string>& open_review_paths,
    const CampaignSpec& spec);

struct SettledSample {
  int atomic{};
  double score{};
};

// "last 50 settled nodes ≥ 90% atomic and average score ≥ 0.8".
bool stop_rule_met(const std::vector<SettledSample>& settled_in_order,
                   std::size_t window = 50);

// Pure cost projection: loop budget × measured average cost per attempt,
// with the expected log growth in bytes per node.
std::string project_cost_usd(std::int64_t loop_budget,
                             std::string_view average_attempt_cost_usd);

// ----------------------------------------------------------------- engine

struct LaneMember {
  std::string device_id;
  double weight{1.0};
};

// The single-device engine: runs one campaign against the state writer,
// reserving budget before every ticket, chunking the loop budget through
// RESERVE records, and stopping on the stop rule.
class Engine {
 public:
  struct Options {
    state::StateWriter& writer;
    gate::SyncAsk ask;
    llm::Budget& budget;    // the device budget (reservations)
    int workers{1};         // tickets in flight (M3 runs one at a time)
    std::string origin{"dev0000000000000001"};
    // Worst case of one ticket: pre-check + max_attempts × max_tokens,
    // priced at the configured highest triplet (set by the caller).
    std::string ticket_worst_case_usd{"0.02"};
    // Optional per-1M-token prices for attempt cost metering (decision
    // 0004: no implicit defaults; node.conf arrives in M5).
    std::optional<onion::llm::PriceSchedule> prices;
    // The device's 20-bit id slot (M4+): child TK ids are allocated from
    // slot × 2^40. 0 keeps the legacy allocation for M3-era tests.
    std::int64_t slot{0};
    // M6: the static member list (device ids and weights). When non-empty,
    // the engine only works tickets whose home lane is this device.
    std::vector<LaneMember> members;
    // M6: this device's id (for lane ownership). Empty = single-device mode.
    std::string self;
  };

  Engine(Options options);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  struct RunResult {
    int settled{};
    int atomic{};
    int leaves{};
    int reviews{};
    int attempts{};
    int calls{};  // LLM completions made during this run
    std::string end_reason;  // stop rule | stopped | paused | drained | budget
  };

  // Blocks until the campaign ends. `external_stop` may be set from another
  // thread (the CLI's stop command).
  RunResult run(const CampaignSpec& spec, std::atomic<bool>* external_stop);

  // Review decisions as record submissions. `accept` settles the node so its
  // children become tickets; `retry` re-queues it with the note injected;
  // `reject` tombstones the node and its children.
  void review_accept(std::string_view path, const CampaignSpec& spec);
  void review_retry(std::string_view path, const CampaignSpec& spec,
                    std::string note);
  void review_reject(std::string_view path, const CampaignSpec& spec);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};


// ------------------------------------------------------------------ lanes

// Home lanes (DESIGN › Work scheduling): score = −w / ln(h), with
// h = (x + 0.5) / 2^64 and x the first 64 bits (big-endian) of
// BLAKE2b-256(node_path ‖ device_id). Higher scores own the lane.
double hrw_score(std::string_view path, std::string_view device_id,
                 double weight);

// The device with the highest HRW score for the path, ties broken by the
// lexicographically smaller device id. Empty members → empty result.
std::optional<std::string> lane_owner(std::string_view path,
                                      const std::vector<LaneMember>& members);

}  // namespace onion::sched
