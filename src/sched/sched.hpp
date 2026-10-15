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
  int light_funnel_from{6};  // decision 0017: light funnel from L6
  bool structure{false};                  // L1..L3 expansion allowed (M3: no)
  bool spillover{true};  // M7: steal from budget-empty lanes when true
  std::string model{"deepseek-flash"};
  std::string critic_model;               // empty = same as model
  bool auto_accept{false};                // M3: parsed, not acted on
  // Decision 0024: the gate is the deterministic checks. `gate`/`floor`
  // above stay parseable for old campaign files and are ignored.
  bool critic{false};          // "on": critique runs and is recorded; never gates
  int children_min{8};         // target lower bound (hard max: synth_max_children, 30)
  double stop_atomic_share{0.90};  // stop rule: verified atomic share threshold
  // W5 funnel variants (absent = the FunnelOptions default): V1 workers'
  // candidates per aspect, V2 planner aspect range, V3 trim rounds, V4
  // synthesizer max children, V5 compact output, V6 shared cache prefix.
  std::optional<int> worker_candidates;
  std::optional<int> aspects_min;
  std::optional<int> aspects_max;
  std::optional<std::vector<int>> trim_keeps;
  std::optional<int> synth_max_children;
  bool compact_output{false};
  bool cache_prefix{true};   // decision 0017: shared prompt prefix on by default
  bool verify_atomic{true};               // decision 0021: batched, on by default
  // M8 branch shares (decision 0020): a child branch may spend at most
  // branch_share of its parent's remaining pool; the pools map (root
  // path -> attempts) is computed at campaign start, embedded in the
  // CAMPAIGN record, and folded by every replica. Empty = the rule is
  // off (pre-M8 campaigns).
  double branch_share{0.5};
  std::map<std::string, std::int64_t> pools;
  // M8 depth checkpoint (DESIGN › Depth checkpoint): a ticket at every
  // `depth_checkpoint_every` levels past `depth_checkpoint_from` is not
  // started — the scheduler writes NEEDS_REVIEW (reason depth
  // checkpoint) without spending an attempt. Both 0 = off.
  int depth_checkpoint_from{0};
  int depth_checkpoint_every{0};
};

std::optional<CampaignSpec> parse_campaign(const nlohmann::json& value,
                                           std::string* error);

// The canonical record form of a spec (decision 0003: no floats — gate,
// floor and every score are exact decimal strings).  Both the campaign
// start command and the engine's CAMPAIGN record use this, so a record
// parsed back by parse_campaign yields the same spec.
nlohmann::json campaign_spec_json(const CampaignSpec& spec);

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

// One settled expansion node (SETTLE with children): how many children it
// kept and how many of them are VERIFIED atomic.
struct SettledSample {
  int kept{};
  int atomic{};
};

// Decision 0024: over the last `window` (50) settled expansion nodes the
// pooled verified-atomic share (Σ atomic / Σ kept) is at least `share`
// (default 0.90). Needs `window` samples. No score clause.
bool stop_rule_met(const std::vector<SettledSample>& settled_in_order,
                   std::size_t window = 50, double share = 0.90);

// Pure cost projection: loop budget × measured average cost per attempt,
// with the expected log growth in bytes per node.
std::string project_cost_usd(std::int64_t loop_budget,
                             std::string_view average_attempt_cost_usd);

// ----------------------------------------------------------------- plan

// Parameters of the campaign plan projection (decision 0016).  Defaults are
// the clearly-labelled fallbacks from OPTIMIZATION_GOAL.md §4.3 and the
// real-API measurements; the store's measured records replace them when
// present.
struct PlanParams {
  double children_kept{40.0};      // average children kept per settled node
  double atomic_share{0.3};        // fraction of children labelled atomic
  double attempts_avg{1.3};        // average attempts per node (with retries)
  double calls_per_attempt{8.0};   // planner + workers + trims + critique + synthesizer
  double precheck_calls{1.0};      // the decomposability pre-check
  double tokens_in_per_call{2200.0};   // input tokens per call
  double tokens_out_per_call{1700.0};  // output tokens per call
  double precheck_tokens_in{200.0};
  double precheck_tokens_out{50.0};
  double cache_hit_ratio{0.0};     // fraction of input tokens billed at the cache rate
  double price_peak_in{0.30};      // USD per 1M tokens (decision 0004)
  double price_peak_cache{0.006};
  double price_peak_out{1.20};
  double price_offpeak_in{0.15};
  double price_offpeak_cache{0.003};
  double price_offpeak_out{0.60};
  // Output tokens per second at the measured stable plateau: 58-65k at
  // 700-800 in flight, about 77k peak at 1,000 (F1).  Time is computed
  // from this rate, not from answers/s, because the funnel's calls average
  // about 1,400 output tokens while the probe's tiny answers were about 365.
  double output_tokens_per_s{60000.0};
  // Current oplog footprint divided by settled nodes.  Empty/new stores use
  // this conservative fallback until enough real records exist to measure it.
  double log_bytes_per_node{2048.0};
};

struct LevelPlan {
  int level{};
  std::int64_t expandable{};
  std::int64_t new_nodes{};
  std::int64_t attempts{};
  std::int64_t calls{};
  std::int64_t tokens_in_miss{};
  std::int64_t tokens_in_hit{};
  std::int64_t tokens_out{};
  double cost_peak{};
  double cost_offpeak{};
  double hours{};
};

struct CampaignPlan {
  std::vector<LevelPlan> levels;
  std::int64_t total_new_nodes{};
  // The number of work nodes the plan will expand.  This is distinct from
  // total_attempts because retries produce more attempts, not more nodes.
  std::int64_t total_expanded_nodes{};
  std::int64_t total_attempts{};
  std::int64_t total_calls{};
  std::int64_t tokens_in_miss{};
  std::int64_t tokens_in_hit{};
  std::int64_t tokens_out{};
  double cost_peak{};
  double cost_offpeak{};
  double hours{};
  bool saturated{false};  // int64 clamped at some level
  double spend_per_hour_peak{};
  double spend_per_hour_offpeak{};
  std::int64_t expected_log_growth_bytes{};
};

// Projects a campaign over `target_depth` levels (levels 0..target_depth are
// expanded; level target_depth+1 receives their children).  `level_nodes`
// gives the starting expandable-node count per level.  With expand_children
// false, only the starting nodes are expanded (their children are created
// but never re-expanded — the "one more level" horizon); with it true (the
// default), children cascade into the next level's expansion, the realistic
// full-campaign horizon.
CampaignPlan plan_campaign(const std::vector<std::int64_t>& level_nodes,
                           int target_depth, const PlanParams& params,
                           bool expand_children = true);

// "Grow N more levels on the whole existing tree": every existing node
// expands once (wave 1) and the cascade continues exactly N waves — the
// closed form sum over waves of growth^i.  Unlike plan_campaign's
// depth target (which re-grows every level's subtree to the target
// depth), this is the faithful N-level horizon the owner's growth
// arithmetic expects (g^N, not g^(depth-L)).
CampaignPlan plan_grow_levels(std::int64_t expandable_total, int levels,
                              const PlanParams& params);

// Fills the measured parameters (children kept, atomic share, attempts and
// tokens per node) from the store's settle and attempt records; every field
// with no measured data keeps its clearly-labelled default.  Prices, the
// rate and the cache-hit ratio always keep their configured values.
PlanParams measure_params(const store::Db& db, std::string* error);

// M8 gate: actual log growth must stay within ±25% of the projection.  A
// zero projection only matches zero observed growth.
bool log_growth_matches_projection(std::int64_t observed_bytes,
                                   const CampaignPlan& plan,
                                   double tolerance = 0.25);

// Renders a plan as a compact text table (for `campaign plan` and the
// `campaign start` confirmation).
std::string plan_report(const CampaignPlan& plan, const PlanParams& params);

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
    // F-GATE-2: after the provider fails (outage, 401/402, 429...) the
    // engine settles nothing for the node and pauses work: the first
    // retry waits `provider_backoff_initial`, doubling per consecutive
    // failure up to `provider_backoff_max`.
    std::chrono::milliseconds provider_backoff_initial{5000};
    std::chrono::milliseconds provider_backoff_max{300000};
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
  // Takes the spec by reference: a NEW campaign gets its starting pools
  // (decision 0020) embedded before the CAMPAIGN record is written.
  RunResult run(CampaignSpec& spec, std::atomic<bool>* external_stop);

  // Decision 0014: the node loop reports a completed sync round; the
  // engine measures the window's attempts (the next chunk is
  // max(50, measured × 1.2)) and re-opens reservations (at most one
  // RESERVE per sync round per device).
  void note_sync_round();

  // M7: the live lane list (the node loop derives it from the SWIM
  // membership — DEAD members drop out once a majority is visible, so
  // their tickets re-hash to the survivors). Replaces the static list.
  void set_lane_members(std::vector<LaneMember> members);
  // The effective lane list: the live one when set, else the static one.
  const std::vector<LaneMember>& live_lane_members() const;

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

// ------------------------------------------------------------------ pools

// Decision 0020: the branch-share pool ledger. Events (a child becoming a
// ticket, an attempt on a node) fold in (hlc, origin) order so every
// replica computes the same allowance in every delivery order.
struct PoolEvent {
  enum class Kind { Create, Attempt } kind;
  std::string path;
  log::Hlc hlc{};
  std::string origin;
};

// allowance(child) = floor(0.5 x remaining(parent)) at the child's
// creation point in the folded order, where remaining(parent) =
// allowance(parent) - attempts spent in the parent's subtree so far.
// A root's allowance is its pool; an allowance below 1 means the node
// pays nothing (leaf, end = allowance spent). Pure: the caller supplies
// the events in ANY order; the fold sorts them.
std::map<std::string, std::int64_t> fold_pool_allowances(
    const std::map<std::string, std::int64_t>& root_pools,
    const std::vector<PoolEvent>& events,
    std::uint64_t imported_before_hlc = 0);

// Decision 0020: starting pools — the campaign's loop budget split
// across its scope roots in proportion to each root's OPEN ticket count
// (a root with none gets one attempt). Uses the same open-ticket rule as
// order_tickets, so the pools are what the engine will actually work.
std::map<std::string, std::int64_t> compute_starting_pools(
    state::StateWriter& writer, const CampaignSpec& spec,
    std::string* error);

}  // namespace onion::sched
