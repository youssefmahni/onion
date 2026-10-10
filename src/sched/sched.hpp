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

}  // namespace onion::sched
