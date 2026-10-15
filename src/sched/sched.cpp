#include "sched.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <deque>
#include <limits>
#include <random>
#include <set>

#include "core/depot_core.hpp"

namespace onion::sched {
namespace {

std::string parent_path(std::string_view path) {
  const std::size_t dash = path.rfind('-');
  return dash == std::string_view::npos ? std::string{} : std::string(path.substr(0, dash));
}

bool in_scope(std::string_view path, const CampaignSpec& spec) {
  if (spec.scope.empty()) return true;
  for (const std::string& prefix : spec.scope) {
    if (path.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

}  // namespace

std::optional<CampaignSpec> parse_campaign(const nlohmann::json& value,
                                           std::string* error) {
  const auto fail = [&](const std::string& what) {
    if (error) *error = what;
    return std::nullopt;
  };
  if (!value.is_object()) return fail("campaign file must be a JSON object");
  CampaignSpec spec;
  if (!value.contains("id") || !value["id"].is_string())
    return fail("campaign id (string) is required");
  spec.id = value["id"].get<std::string>();
  if (value.contains("scope")) {
    if (!value["scope"].is_array()) return fail("scope must be an array");
    for (const auto& item : value["scope"]) {
      if (!item.is_string()) return fail("scope entries must be strings");
      spec.scope.push_back(item.get<std::string>());
    }
  }
  spec.loop_budget = value.value("loop_budget", std::int64_t{10000});
  spec.mode = value.value("mode", std::string{"depth-first"});
  if (spec.mode != "breadth-first" && spec.mode != "depth-first")
    return fail("mode must be breadth-first or depth-first");
  // Records never contain floats (decision 0003): scores are exact decimal
  // strings, so gate/floor accept a number or a numeric string.
  const auto parse_score = [&](const char* key, double fallback) -> double {
    if (!value.contains(key)) return fallback;
    if (value[key].is_number())
      return value[key].get<double>();
    if (value[key].is_string())
      return std::stod(value[key].get<std::string>());
    throw std::runtime_error(std::string(key) + " must be a number or a "
                             "numeric string");
  };
  try {
    spec.gate = parse_score("gate", 0.7);
    spec.floor = parse_score("floor", 0.5);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  spec.max_attempts = value.value("max_attempts", 3);
  spec.chunk = value.value("chunk", std::int64_t{50});
  // Decision 0017 supersedes the original M3 L8 default.  Keep this in
  // lockstep with CampaignSpec so a campaign file and its persisted spec
  // select the same funnel at L6.
  spec.light_funnel_from = value.value("light_funnel_from", 6);
  spec.structure = value.value("structure", false);
  spec.review_all = value.value("review_all", false);
  spec.spillover = value.value("spillover", true);
  spec.model = value.value("model", std::string{"deepseek-flash"});
  spec.critic_model = value.value("critic_model", std::string{});
  spec.auto_accept = value.value("auto_accept", false);
  spec.tree = value.value("tree", std::string{});
  spec.profile_hash = value.value("profile_hash", std::string{});
  // Decision 0024. critic: "on" | "off" (a boolean is accepted too).
  if (value.contains("critic")) {
    if (value["critic"].is_boolean())
      spec.critic = value["critic"].get<bool>();
    else if (value["critic"].is_string() &&
             (value["critic"].get<std::string>() == "on" ||
              value["critic"].get<std::string>() == "off"))
      spec.critic = value["critic"].get<std::string>() == "on";
    else
      return fail("critic must be \"on\" or \"off\"");
  }
  if (value.contains("children_min")) {
    if (!value["children_min"].is_number_integer())
      return fail("children_min must be an integer");
    const std::int64_t v = value["children_min"].get<std::int64_t>();
    if (v < 3 || v > 30) return fail("children_min out of range 3..30");
    spec.children_min = static_cast<int>(v);
  }
  try {
    spec.stop_atomic_share = parse_score("stop_atomic_share", 0.90);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  if (spec.stop_atomic_share <= 0.0 || spec.stop_atomic_share > 1.0)
    return fail("stop_atomic_share out of range (0, 1]");
  // M8 branch share (decision 0020): a number or an exact decimal string
  // (decision 0003 — no floats in records).
  try {
    spec.branch_share = parse_score("branch_share", 0.5);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  if (spec.branch_share <= 0.0 || spec.branch_share > 1.0)
    return fail("branch_share must be in (0, 1]");
  if (value.contains("pools") && value["pools"].is_object())
    for (const auto& [path, attempts] : value["pools"].items())
      if (attempts.is_number_integer())
        spec.pools[path] = attempts.get<std::int64_t>();
  spec.depth_checkpoint_from = value.value("depth_checkpoint_from", 0);
  spec.depth_checkpoint_every = value.value("depth_checkpoint_every", 0);
  if (spec.depth_checkpoint_from < 0 || spec.depth_checkpoint_every < 0)
    return fail("depth checkpoint fields must be non-negative");
  if ((spec.depth_checkpoint_from == 0) != (spec.depth_checkpoint_every == 0))
    return fail("depth checkpoint needs both from and every (or neither)");
  // M8 branch share (decision 0020): a number or an exact decimal string
  // (decision 0003 — no floats in records).
  try {
    spec.branch_share = parse_score("branch_share", 0.5);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  if (spec.branch_share <= 0.0 || spec.branch_share > 1.0)
    return fail("branch_share must be in (0, 1]");
  if (value.contains("pools") && value["pools"].is_object())
    for (const auto& [path, attempts] : value["pools"].items())
      if (attempts.is_number_integer())
        spec.pools[path] = attempts.get<std::int64_t>();
  spec.depth_checkpoint_from = value.value("depth_checkpoint_from", 0);
  spec.depth_checkpoint_every = value.value("depth_checkpoint_every", 0);
  if (spec.depth_checkpoint_from < 0 || spec.depth_checkpoint_every < 0)
    return fail("depth checkpoint fields must be non-negative");
  if ((spec.depth_checkpoint_from == 0) != (spec.depth_checkpoint_every == 0))
    return fail("depth checkpoint needs both from and every (or neither)");
  // W5 funnel variants: optional; every value is validated on parse.
  if (value.contains("worker_candidates")) {
    if (!value["worker_candidates"].is_number_integer())
      return fail("worker_candidates must be an integer");
    const std::int64_t v = value["worker_candidates"].get<std::int64_t>();
    if (v < 1 || v > 500) return fail("worker_candidates out of range 1..500");
    spec.worker_candidates = static_cast<int>(v);
  }
  if (value.contains("aspects_min") || value.contains("aspects_max")) {
    const std::int64_t min =
        value.value("aspects_min", std::int64_t{3});
    const std::int64_t max =
        value.value("aspects_max", std::int64_t{5});
    if (min < 2 || max < min || max > 20)
      return fail("aspects range invalid (2 <= min <= max <= 20)");
    spec.aspects_min = static_cast<int>(min);
    spec.aspects_max = static_cast<int>(max);
  }
  if (value.contains("trim_keeps")) {
    if (!value["trim_keeps"].is_array() || value["trim_keeps"].empty())
      return fail("trim_keeps must be a non-empty array of integers");
    std::vector<int> keeps;
    for (const auto& keep : value["trim_keeps"]) {
      if (!keep.is_number_integer())
        return fail("trim_keeps entries must be integers");
      const std::int64_t k = keep.get<std::int64_t>();
      if (k < 10 || k > 1000)
        return fail("trim_keeps entries out of range 10..1000");
      keeps.push_back(static_cast<int>(k));
    }
    spec.trim_keeps = std::move(keeps);
  }
  if (value.contains("synth_max_children")) {
    if (!value["synth_max_children"].is_number_integer())
      return fail("synth_max_children must be an integer");
    const std::int64_t v = value["synth_max_children"].get<std::int64_t>();
    if (v < 3 || v > 100)
      return fail("synth_max_children out of range 3..100");
    spec.synth_max_children = static_cast<int>(v);
  }
  spec.compact_output = value.value("compact_output", false);
  spec.cache_prefix = value.value("cache_prefix", true);
  spec.verify_atomic = value.value("verify_atomic", true);
  if (spec.loop_budget < 1) return fail("loop_budget must be positive");
  if (spec.chunk < 1) return fail("chunk must be positive");
  return spec;
}

nlohmann::json campaign_spec_json(const CampaignSpec& spec) {
  nlohmann::json out{
      {"id", spec.id},
      {"scope", spec.scope},
      {"loop_budget", spec.loop_budget},
      {"mode", spec.mode},
      {"gate", std::to_string(spec.gate)},
      {"floor", std::to_string(spec.floor)},
      {"max_attempts", spec.max_attempts},
      {"chunk", spec.chunk},
      {"light_funnel_from", spec.light_funnel_from},
      {"structure", spec.structure},
      {"spillover", spec.spillover},
      {"model", spec.model},
      {"critic_model", spec.critic_model},
      {"auto_accept", spec.auto_accept},
      {"critic", spec.critic ? "on" : "off"},
      {"children_min", spec.children_min},
      {"stop_atomic_share", std::to_string(spec.stop_atomic_share)}};
  if (spec.worker_candidates)
    out["worker_candidates"] = *spec.worker_candidates;
  if (spec.aspects_min) out["aspects_min"] = *spec.aspects_min;
  if (spec.aspects_max) out["aspects_max"] = *spec.aspects_max;
  if (spec.trim_keeps) out["trim_keeps"] = *spec.trim_keeps;
  if (spec.synth_max_children)
    out["synth_max_children"] = *spec.synth_max_children;
  if (spec.compact_output) out["compact_output"] = true;
  if (spec.cache_prefix) out["cache_prefix"] = true;
  if (spec.verify_atomic) out["verify_atomic"] = true;
  // M8 branch share (decision 0020): the ratio and the starting pools
  // travel in the record so every replica folds the same numbers.
  out["branch_share"] = std::to_string(spec.branch_share);
  if (!spec.tree.empty()) out["tree"] = spec.tree;
  if (!spec.profile_hash.empty()) out["profile_hash"] = spec.profile_hash;
  // Emitted only when set, so a planning campaign's record keeps its
  // historical shape.
  if (spec.review_all) out["review_all"] = true;
  if (spec.depth_checkpoint_every > 0) {
    out["depth_checkpoint_from"] = spec.depth_checkpoint_from;
    out["depth_checkpoint_every"] = spec.depth_checkpoint_every;
  }
  if (!spec.pools.empty()) {
    nlohmann::json pools = nlohmann::json::object();
    for (const auto& [path, attempts] : spec.pools) pools[path] = attempts;
    out["pools"] = std::move(pools);
  }
  return out;
}

std::vector<Ticket> order_tickets(
    const std::vector<store::Node>& nodes,
    const std::function<bool(std::string_view)>& settled,
    const std::function<bool(std::string_view)>& leaf,
    const std::vector<std::string>& open_review_paths,
    const CampaignSpec& spec) {
  const auto under_open_review = [&](std::string_view path) {
    for (const std::string& review : open_review_paths) {
      if (review.empty()) continue;
      if (path == review || path.rfind(review + "-", 0) == 0) return true;
    }
    return false;
  };
  std::vector<Ticket> tickets;
  for (const store::Node& node : nodes) {
    if (node.tombstone) continue;
    if (!in_scope(node.path, spec)) continue;
    if (settled(node.path)) continue;
    if (leaf(node.path)) continue;
    if (under_open_review(node.path)) continue;
    tickets.push_back(Ticket{node.path, node.title, node.layer, spec.id});
  }
  std::sort(tickets.begin(), tickets.end(),
            [&](const Ticket& a, const Ticket& b) {
              if (a.layer != b.layer)
                return spec.mode == "depth-first" ? a.layer > b.layer
                                                  : a.layer < b.layer;
              return a.path < b.path;
            });
  return tickets;
}

bool stop_rule_met(const std::vector<SettledSample>& settled_in_order,
                   std::size_t window, double share) {
  if (settled_in_order.size() < window) return false;
  const std::size_t begin = settled_in_order.size() - window;
  std::int64_t kept = 0;
  std::int64_t atomic = 0;
  for (std::size_t i = begin; i < settled_in_order.size(); ++i) {
    kept += settled_in_order[i].kept;
    atomic += settled_in_order[i].atomic;
  }
  if (kept <= 0) return false;
  return static_cast<double>(atomic) / static_cast<double>(kept) >= share;
}

std::string project_cost_usd(std::int64_t loop_budget,
                             std::string_view average_attempt_cost_usd) {
  const auto parse = [](std::string_view value) -> std::int64_t {
    std::string cleaned(value);
    std::size_t dot = cleaned.find('.');
    std::string integer = dot == std::string::npos ? cleaned : cleaned.substr(0, dot);
    std::string fraction =
        dot == std::string::npos ? "0" : cleaned.substr(dot + 1);
    if (fraction.size() < 9) fraction.append(9 - fraction.size(), '0');
    return std::stoll(integer + fraction.substr(0, 9), nullptr, 10);
  };
  const std::int64_t cents = parse(average_attempt_cost_usd);
  std::int64_t total = 0;
  for (std::int64_t i = 0; i < loop_budget; ++i) {
    total += cents;
    if (total > std::numeric_limits<std::int64_t>::max() / 2) break;
  }
  const std::string text = std::to_string(total);
  if (text.size() <= 9)
    return "0." + std::string(9 - text.size(), '0') + text;
  return text.substr(0, text.size() - 9) + "." + text.substr(text.size() - 9);
}

// ------------------------------------------------------------------ ids

NodeIdAllocator::NodeIdAllocator(std::int64_t slot, std::int64_t next_counter)
    : slot_(slot), counter_(next_counter) {}

std::int64_t NodeIdAllocator::id_value(std::int64_t slot, std::int64_t counter) {
  return (slot << 40) + counter;
}

std::string NodeIdAllocator::allocate() {
  const std::int64_t value = id_value(slot_, counter_++);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "TK%05lld", static_cast<long long>(value));
  return buffer;
}

std::int64_t NodeIdAllocator::counter_from_code(std::string_view code) {
  if (code.size() < 3 || code.substr(0, 2) != "TK") return 0;
  std::int64_t value = 0;
  for (const char c : code.substr(2)) {
    if (c < '0' || c > '9') return 0;
    value = value * 10 + (c - '0');
  }
  return value;
}

// ----------------------------------------------------------------- engine

namespace {
std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

log::Record make_record(std::string type, nlohmann::json payload) {
  log::Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  return record;
}
}  // namespace

struct Engine::Impl {
  Options options;
  std::vector<LaneMember> lane_members;  // the live list (M7)
  std::atomic<bool> internal_stop{false};
  // Decision 0014: sync-round pacing for fleet reservations, per campaign
  // (one Engine runs every campaign, so the round state must not leak
  // between them).
  struct RoundState {
    // Decision 0014 amendment (2026-10-13): closed by default. The old
    // "first reservation is free" rule let a device that restarted inside
    // a partition reserve once without any completed round; now a fleet
    // device reserves only after a MAJORITY round (the node loop's
    // majority_round verdict). A fresh RoundState opens immediately ONLY
    // for a fleet of one (0/1 is a majority).
    bool sync_round_complete{false};
    std::int64_t attempts_in_window{0};
    std::int64_t last_window_attempts{0};
  };
  std::map<std::string, RoundState> round_states;
  // M7 stealing: paths we claimed — campaign plus the claim's local
  // receipt time (renewed every 3 minutes while the ticket is open).
  std::map<std::string, std::pair<std::string, std::int64_t>> stolen_claims;
  std::mt19937 rng_{std::random_device{}()};
  std::map<std::string, std::vector<log::Hlc>> own_reserve_hlcs;
  log::HlcClock hlc;
  // Decision 0025: consecutive settles with zero attempts and zero
  // spend — the churn guard pauses the campaign after 50 of them
  // (reason "no progress"). Persists across run() re-entries.
  int zero_attempt_settles{0};
  // M8 pre-work (the sync scheduler thread): note_sync_round() runs on
  // the scheduler thread while run() runs on the node loop thread. This
  // mutex guards round_states and stolen_claims. Lock order (the ONLY
  // one): pacing_mutex_ is never held while performing LLM I/O; the
  // writer calls under it are queue submissions and shared-index reads,
  // which never call back into the engine.
  std::mutex pacing_mutex_;
  // F-GATE-2: consecutive provider failures and the time before which the
  // engine starts no new ticket (node loop thread only).
  int provider_failures{0};
  std::chrono::steady_clock::time_point provider_backoff_until{};
};

Engine::Engine(Options options)
    : impl_(new Impl{std::move(options), {}, {false}}) {}
Engine::~Engine() = default;

void Engine::set_lane_members(std::vector<LaneMember> members) {
  impl_->lane_members = std::move(members);
}

const std::vector<LaneMember>& Engine::live_lane_members() const {
  // The live list when set, the static one otherwise.
  return impl_->lane_members.empty() && !impl_->options.members.empty()
             ? impl_->options.members
             : impl_->lane_members;
}

void Engine::note_sync_round() {
  std::lock_guard pacing(impl_->pacing_mutex_);
  for (auto& [campaign, state] : impl_->round_states) {
    state.last_window_attempts = state.attempts_in_window;
    state.attempts_in_window = 0;
    state.sync_round_complete = true;
  }
  // M7 stealing: renew every claim older than 3 minutes whose ticket is
  // still open (a settle ends the claim in the fold).
  const std::int64_t now_ms = wall_ms();
  for (auto it = impl_->stolen_claims.begin();
       it != impl_->stolen_claims.end();) {
    const std::string path = it->first;
    const auto campaign_state =
        impl_->options.writer.campaign(it->second.first);
    const bool campaign_over =
        !campaign_state || campaign_state->state != "running";
    if (impl_->options.writer.settled(path) || campaign_over) {
      it = impl_->stolen_claims.erase(it);
      continue;
    }
    if (now_ms - it->second.second >= 3 * 60 * 1000) {
      impl_->options.writer.submit(state::CommitTerminal{{make_record(
          "CLAIM", {{"campaign", it->second.first},
                    {"path", path},
                    {"lease_s", 600}})}});
      it->second.second = now_ms;
    }
    ++it;
  }
}

namespace {

// Walks the node's ancestor chain by path prefix and collects the context
// the gate checks need.
gate::NodeContext build_context(state::StateWriter& writer,
                                const store::Node& node,
                                std::string* error) {
  gate::NodeContext context;
  context.path = node.path;
  context.title = node.title;
  context.layer = node.layer;

  std::vector<std::string> ancestor_paths;
  std::string path = parent_path(node.path);
  while (!path.empty()) {
    ancestor_paths.push_back(path);
    path = parent_path(path);
  }
  std::reverse(ancestor_paths.begin(), ancestor_paths.end());
  for (const std::string& ancestor : ancestor_paths) {
    const auto found = writer.db().node_by_path(ancestor, error);
    if (found) context.ancestors.push_back(depot::canonical(found->title));
  }

  const std::string parent = parent_path(node.path);
  if (!parent.empty()) {
    for (const store::Node& sibling :
         writer.db().nodes_with_path_prefix(parent, error)) {
      if (sibling.path == node.path) continue;
      if (parent_path(sibling.path) == parent)
        context.siblings.push_back(depot::canonical(sibling.title));
    }
  }
  for (const store::Node& child :
       writer.db().nodes_with_path_prefix(node.path, error)) {
    if (parent_path(child.path) == node.path)
      context.existing.push_back(depot::canonical(child.title));
  }
  return context;
}

std::string attempt_issues(const gate::Attempt& attempt) {
  return attempt.issues;
}

}  // namespace

Engine::RunResult Engine::run(CampaignSpec& spec,
                              std::atomic<bool>* external_stop) {
  RunResult result;
  auto& writer = impl_->options.writer;
  const auto existing = writer.campaign(spec.id);
  if (existing && existing->state != "running") {
    result.end_reason = existing->state;
    return result;
  }

  // Emit the CAMPAIGN record only when the campaign is new (the run loop
  // re-enters run() after every drain; re-emission would spam the log).
  if (!existing) {
    // M8 branch shares (decision 0020): the starting pools are computed
    // HERE, once, and embedded in the record so every replica folds the
    // same numbers. A pre-M8 campaign (pools already absent from a
    // stored spec) stays without the rule.
    if (spec.pools.empty() && !spec.scope.empty()) {
      std::string pool_error;
      spec.pools =
          compute_starting_pools(writer, spec, &pool_error);
    }
    writer.submit(state::CommitTerminal{{make_record(
      "CAMPAIGN", {{"id", spec.id},
                   {"spec", campaign_spec_json(spec)},
                   {"state", "running"}})}});
    // Fold it before any further decision: with the 0014 amendment the
    // first run can drain on a closed gate WITHOUT reserving, and the
    // campaign's existence must not depend on that reservation's sync.
    writer.sync();
  }

  gate::FunnelOptions funnel_options;
  // Decision 0027: resolve the campaign's domain profile. Empty hash =
  // the embedded planning profile (byte-identical to the historical
  // hard-coded rules). A device that does not hold the exact profile
  // does not run the campaign: pause it with a clear reason.
  onion::domain::Profile profile = onion::domain::planning_profile();
  const std::string planning_profile_hash =
      onion::domain::profile_hash(profile);
  if (!spec.profile_hash.empty() && spec.profile_hash != planning_profile_hash) {
    std::string profile_error;
    const auto held =
        writer.db().domain_profile(spec.profile_hash, &profile_error);
    if (!held) {
      writer.submit(state::CommitTerminal{{make_record(
          "CAMPAIGN", {{"id", spec.id},
                       {"spec", campaign_spec_json(spec)},
                       {"state", "paused"},
                       {"reason", "profile " + spec.profile_hash +
                                      " not held"}})}});
      writer.sync();
      result.end_reason = "profile missing";
      return result;
    }
    const auto parsed = onion::domain::parse_profile(
        nlohmann::json::parse(held->json, nullptr, false), &profile_error);
    if (!parsed) {
      writer.submit(state::CommitTerminal{{make_record(
          "CAMPAIGN", {{"id", spec.id},
                       {"spec", campaign_spec_json(spec)},
                       {"state", "paused"},
                       {"reason", "profile parse failed: " +
                                      profile_error}})}});
      writer.sync();
      result.end_reason = "profile invalid";
      return result;
    }
    profile = *parsed;
  }
  funnel_options.profile = profile;
  funnel_options.params.max_attempts = spec.max_attempts;
  funnel_options.params.children_min = spec.children_min;
  funnel_options.critic = spec.critic;
  funnel_options.model = spec.model;
  funnel_options.critic_model = spec.critic_model;  // the critique call only
  funnel_options.prices = impl_->options.prices;
  // W5 funnel variants (absent spec fields keep the FunnelOptions default).
  if (spec.worker_candidates)
    funnel_options.worker_candidates = *spec.worker_candidates;
  if (spec.aspects_min) funnel_options.aspects_min = *spec.aspects_min;
  if (spec.aspects_max) funnel_options.aspects_max = *spec.aspects_max;
  if (spec.trim_keeps) funnel_options.trim_keeps = *spec.trim_keeps;
  if (spec.synth_max_children)
    funnel_options.synth_max_children = *spec.synth_max_children;
  funnel_options.compact_output = spec.compact_output;
  funnel_options.verify_atomic = spec.verify_atomic;
  // The planning cache prefix names planning-domain noun phrases. A signed
  // profile owns its own prompt contract, so never inject that stale prefix
  // into a non-planning campaign.
  if (spec.cache_prefix &&
      !(profile.document.is_object() && profile.document.contains("prompts")))
    funnel_options.cache_prefix =
        // Shared fixed block (~120 tokens): identical for every funnel
        // call, so the provider's automatic prefix cache can hit on every
        // call after the first (W5 V6).
        "You build a hierarchical AI-agent knowledge base. Nodes decompose "
        "into direct children (Title Case noun phrases, 2-5 words). Strict "
        "JSON only: no prose, no markdown fences. Follow the instruction "
        "below exactly for this node.";

  std::vector<SettledSample> settled_samples;
  std::deque<Ticket> waiting;
  std::unique_ptr<NodeIdAllocator> node_ids_;
  std::int64_t used_in_chunk = 0;
  std::int64_t current_chunk_size_ = 0;

  const auto stop_requested = [&] {
    if (external_stop && external_stop->load()) return true;
    if (impl_->internal_stop.load()) return true;
    const auto campaign = writer.campaign(spec.id);
    return campaign && campaign->state != "running";
  };

  // Child ids come from the device's slot (slot × 2^40 + counter).
  {
    std::string id_error;
    const std::int64_t max_tk = writer.db().max_tk_value(&id_error);
    std::int64_t next_counter = 0;
    if (impl_->options.slot > 0) {
      next_counter =
          max_tk >= (impl_->options.slot << 40)
              ? max_tk - (impl_->options.slot << 40) + 1
              : 0;
    } else {
      next_counter = max_tk + 1;  // slot 0: keep allocating the legacy way
    }
    node_ids_ = std::make_unique<NodeIdAllocator>(impl_->options.slot,
                                                  next_counter);
  }

  // Local node cache: the engine is the only writer of new nodes, so the
  // cache is refreshed from SQLite only when the snapshot drains; commits
  // append their children directly. This keeps the ticket index O(n log n)
  // instead of one full store query per ticket.
  std::vector<store::Node> node_cache;
  std::vector<std::string> review_cache;
  bool cache_valid = false;
  // M8 branch shares (decision 0020): the allowance fold and the
  // attempt list, rebuilt with the ticket cache. Deterministic: both
  // come from the store (itself a fold), ordered by (hlc, origin) inside
  // fold_pool_allowances. Empty spec.pools (pre-M8 campaigns) disables
  // the rule.
  std::map<std::string, std::int64_t> allowances;
  std::vector<std::string> attempt_paths;
  std::unordered_set<std::string> imported_paths;
  const auto refresh_tickets = [&]() {
    std::string error;
    if (!cache_valid) {
      node_cache.clear();
      if (spec.scope.empty())
        node_cache = writer.db().all_nodes(&error);
      else
        for (const std::string& prefix : spec.scope)
          for (const store::Node& node :
               writer.db().nodes_with_path_prefix(prefix, &error))
            node_cache.push_back(node);
      review_cache.clear();
      for (const store::Review& review : writer.db().open_reviews(&error))
        if (review.state == "open" || review.state.empty())
          review_cache.push_back(review.path);
      cache_valid = true;
    }
    // The pool ledger is rebuilt on EVERY refresh: children join the
    // node cache incrementally as settles commit, and each new attempt
    // changes the remaining pools. The events come from the STORE, never
    // the cache: the cache's incremental entries carry no hlc (they would
    // sort before every attempt and break the remaining-pool math).
    allowances.clear();
    attempt_paths.clear();
    imported_paths.clear();
    // Decision 0025/0027: imported tickets (nodes with source "import")
    // start a fresh chain and are NEVER leafed as "allowance spent".
    if (!spec.pools.empty()) {
      std::vector<PoolEvent> events;
      std::string nodes_error;
      if (spec.scope.empty()) {
        for (const store::Node& node : writer.db().all_nodes(&nodes_error)) {
          events.push_back(PoolEvent{PoolEvent::Kind::Create, node.path,
                                     node.hlc, node.origin, node.source});
          if (node.source == "import") imported_paths.insert(node.path);
        }
      } else {
        for (const std::string& prefix : spec.scope)
          for (const store::Node& node : writer.db().nodes_with_path_prefix(
                   prefix, &nodes_error)) {
            events.push_back(PoolEvent{PoolEvent::Kind::Create, node.path,
                                       node.hlc, node.origin, node.source});
            if (node.source == "import") imported_paths.insert(node.path);
          }
      }
      std::string attempt_error;
      auto statement = writer.db().prepare(
          "SELECT path, hlc, origin FROM attempts "
          "WHERE campaign = ?1;",
          &attempt_error);
      if (statement) {
        std::string bind_error;
        statement->bind(1, spec.id, &bind_error);
        std::string step_error;
        while (statement->step_row(&step_error)) {
          const std::string path = statement->text(0);
          events.push_back(PoolEvent{PoolEvent::Kind::Attempt, path,
                                     static_cast<std::uint64_t>(
                                         statement->integer(1)),
                                     statement->text(2)});
          attempt_paths.push_back(path);
        }
      }
      allowances = fold_pool_allowances(spec.pools, events);
    }
    return order_tickets(node_cache,
                         [&](std::string_view path) { return writer.settled(path); },
                         [&](std::string_view path) { return writer.leaf(path); },
                         review_cache, spec);
  };
  // The ticket's share: attempts in its subtree (the prefix rule).
  const auto subtree_attempts = [&](std::string_view path) {
    const std::string prefix = std::string(path) + "-";
    std::int64_t total = 0;
    for (const std::string& attempted : attempt_paths)
      if (attempted == path || attempted.rfind(prefix, 0) == 0) ++total;
    return total;
  };

  std::int64_t now_ms = wall_ms();
  // M7 stealing: choose one ticket from the far end of the top-5
  // busiest eligible lanes. Eligibility: the lane's owner must be in the
  // live member list (dead lanes are already re-hashed or untouched by a
  // split), the ticket unclaimed, no open re-expansion, and — when the
  // campaign disallows spillover — the owner's budget must not be
  // exhausted.
  const auto pick_stolen_ticket = [&](const CampaignSpec& spec,
                                      const std::vector<LaneMember>& members)
      -> std::optional<Ticket> {
    std::string error;
    std::vector<store::Node> nodes;
    if (spec.scope.empty())
      nodes = writer.db().all_nodes(&error);
    else
      for (const std::string& prefix : spec.scope)
        for (const store::Node& node :
             writer.db().nodes_with_path_prefix(prefix, &error))
          nodes.push_back(node);
    const auto all_tickets = order_tickets(
        nodes,
        [&](std::string_view path) { return writer.settled(path); },
        [&](std::string_view path) { return writer.leaf(path); },
        review_cache, spec);
    // Group open tickets by lane owner; skip claims, locks and our own
    // lanes.
    const auto owner_reserved = writer.reserved_by_origin(spec.id);
    std::map<std::string, std::vector<Ticket>> lanes;
    if (std::getenv("ONION_SYNC_DEBUG")) {
      std::size_t self_skip = 0, claim_skip = 0, reexpand_skip = 0,
                  no_owner = 0, kept = 0;
      for (const Ticket& ticket : all_tickets) {
        const auto owner = lane_owner(ticket.path, members);
        if (!owner) { no_owner++; continue; }
        if (*owner == impl_->options.self) { self_skip++; continue; }
        if (!writer.claim_holder(ticket.path, impl_->options.self, now_ms)
                 .empty()) { claim_skip++; continue; }
        if (writer.reexpand_open(ticket.path)) { reexpand_skip++; continue; }
        kept++;
      }
      std::fprintf(stderr,
                   "steal debug: all=%zu no_owner=%zu self=%zu claim=%zu "
                   "reexpand=%zu kept=%zu members=%zu\n",
                   all_tickets.size(), no_owner, self_skip, claim_skip,
                   reexpand_skip, kept, members.size());
    }
    for (const Ticket& ticket : all_tickets) {
      const auto owner = lane_owner(ticket.path, members);
      if (!owner || *owner == impl_->options.self) continue;
      {
        // Guarded with the same mutex as note_sync_round() (the sync
        // scheduler thread renews and prunes claims there).
        std::lock_guard pacing(impl_->pacing_mutex_);
        if (impl_->stolen_claims.count(ticket.path)) continue;  // ours
      }
      if (!writer.claim_holder(ticket.path, impl_->options.self, now_ms)
               .empty())
        continue;
      if (writer.reexpand_open(ticket.path)) continue;
      if (!spec.spillover) {
        const auto reserved = owner_reserved.find(*owner);
        if (reserved != owner_reserved.end() &&
            reserved->second >= spec.loop_budget)
          continue;  // budget-empty lane: spillover disallowed
      }
      lanes[*owner].push_back(ticket);
    }
    // The top 5 busiest lanes by open-ticket count.
    std::vector<std::pair<std::string, std::vector<Ticket>>> ranked;
    for (auto& [owner, lane] : lanes)
      ranked.push_back({owner, std::move(lane)});
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                return a.second.size() > b.second.size();
              });
    if (ranked.size() > 5) ranked.resize(5);
    // From each ranked lane take the far end (breadth-first order ends
    // with the tickets the owner would reach last), and pick one of the
    // candidates at random so stealers do not collide.
    std::vector<Ticket> candidates;
    for (const auto& [owner, lane] : ranked)
      if (!lane.empty()) candidates.push_back(lane.back());
    if (candidates.empty()) return std::nullopt;
    std::shuffle(candidates.begin(), candidates.end(), impl_->rng_);
    return candidates.front();
  };

  for (;;) {
    now_ms = wall_ms();  // claims expire per iteration, not per entry
    if (stop_requested()) {
      result.end_reason = "stopped";
      break;
    }
    if (stop_rule_met(settled_samples, 50, spec.stop_atomic_share)) {
      result.end_reason = "stop rule";
      break;
    }
    // Decision 0025 churn guard: 50 consecutive settles with zero
    // attempts and zero spend means the engine is settling without
    // working — pause the campaign (resumable) with a visible reason.
    if (impl_->zero_attempt_settles >= 50) {
      writer.submit(state::CommitTerminal{{make_record(
          "CAMPAIGN", {{"id", spec.id},
                       {"spec", campaign_spec_json(spec)},
                       {"state", "paused"},
                       {"reason", "no progress"}})}});
      writer.sync();
      result.end_reason = "paused";
      break;
    }
    // F-GATE-2: the provider failed recently; start nothing until the
    // backoff elapses (the node loop re-enters run() on its own clock).
    if (std::chrono::steady_clock::now() < impl_->provider_backoff_until) {
      result.end_reason = "provider unavailable";
      break;
    }
    std::vector<Ticket> tickets = refresh_tickets();
    // M6: only the tickets whose home lane is this device are worked; the
    // rest stay for their owners.
    const auto& live_members = live_lane_members();
    const bool fleet = !impl_->options.self.empty() && !live_members.empty();
    if (fleet) {
      tickets.erase(
          std::remove_if(tickets.begin(), tickets.end(),
                         [&](const Ticket& ticket) {
                           const auto owner =
                               lane_owner(ticket.path, live_members);
                           return !owner || *owner != impl_->options.self;
                         }),
          tickets.end());
    }
    // M7 stealing: skip tickets claimed by another device (the lease is
    // timed from OUR receipt — never the claimer's clock) and tickets
    // under an open re-expansion lock.
    bool stole_this_round = false;
    tickets.erase(
        std::remove_if(tickets.begin(), tickets.end(),
                       [&](const Ticket& ticket) {
                         if (!writer.claim_holder(ticket.path,
                                                  impl_->options.self,
                                                  now_ms)
                                  .empty())
                           return true;
                         if (writer.reexpand_open(ticket.path)) return true;
                         return false;
                       }),
        tickets.end());
    if (tickets.empty() && waiting.empty()) cache_valid = false;
    // Decision 0014: a device whose own reservation folded past the
    // budget cap starts no new attempts for the campaign (in-flight
    // attempts finish).
    if (!impl_->options.self.empty() && !live_lane_members().empty()) {
      const auto hlcs = impl_->own_reserve_hlcs.find(spec.id);
      if (hlcs != impl_->own_reserve_hlcs.end() && !hlcs->second.empty() &&
          writer.reservation_void(spec.id, impl_->options.origin,
                                  hlcs->second.back())) {
        result.end_reason = "budget";
        break;
      }
    }
    // A lane down to fewer tickets than its workers steals from the far
    // end of the busiest eligible lanes (DESIGN › Stealing). The void
    // check runs FIRST: a budget-capped device must not hold stolen
    // tickets hostage. The reservation gate runs BEFORE the claim too
    // (0014 amendment): claiming a ticket the device cannot spend on yet
    // would strand it — the next steal scan skips our own claims.
    if (fleet && tickets.empty() && waiting.empty()) {
      bool round_complete = true;
      {
        std::lock_guard pacing(impl_->pacing_mutex_);
        auto [it, inserted] = impl_->round_states.try_emplace(spec.id);
        if (inserted) {
          it->second.sync_round_complete =
              (live_lane_members().size() <= 1);
        }
        round_complete = it->second.sync_round_complete;
      }
      if (!round_complete) {
        result.end_reason = "drained";
        break;
      }
      const auto stolen = pick_stolen_ticket(spec, live_members);
      if (stolen) {
        writer.submit(state::CommitTerminal{{make_record(
            "CLAIM", {{"campaign", spec.id},
                      {"path", stolen->path},
                      {"lease_s", 600}})}});
        writer.sync();
        {
          std::lock_guard pacing(impl_->pacing_mutex_);
          impl_->stolen_claims[stolen->path] = {spec.id, now_ms};
        }
        tickets.push_back(*stolen);
        stole_this_round = true;
      }
    }
    // M8 depth checkpoint (DESIGN › Depth checkpoint): a ticket at a
    // checkpoint layer is NOT started — the scheduler writes
    // NEEDS_REVIEW (reason depth checkpoint) without spending an
    // attempt, and accepting it (REVIEW decide) releases the ticket
    // until the next checkpoint.
    if (spec.depth_checkpoint_every > 0) {
      for (auto it = tickets.begin(); it != tickets.end();) {
        const bool at_checkpoint =
            it->layer >= spec.depth_checkpoint_from &&
            (it->layer - spec.depth_checkpoint_from) %
                    spec.depth_checkpoint_every ==
                0;
        // A DECIDED review means the operator released this node after
        // its checkpoint pause: attempt it now, do not re-pause it.
        std::string review_error;
        const auto prior =
            at_checkpoint
                ? writer.db().review_state(it->path, &review_error)
                : std::nullopt;
        if (at_checkpoint && (!prior || *prior != "decided")) {
          writer.submit(state::CommitTerminal{{make_record(
              "NEEDS_REVIEW", {{"path", it->path},
                               {"reason", "depth checkpoint"},
                               {"attempts", 0},
                               {"score", "0"}})}});
          writer.sync();
          review_cache.push_back(it->path);
          result.reviews++;
          it = tickets.erase(it);
          continue;
        }
        ++it;
      }
    }
    // M8 branch shares (decision 0020): a ticket may start only when
    // EVERY ancestor (the scope root's pool included) can still pay one
    // more attempt — a spent allowance anywhere on the chain, or a
    // folded allowance below 1, leafs it WITHOUT spending (end =
    // allowance spent), exactly like the cannot-split leaf. Runs after
    // the steal so stolen tickets are checked too.
    if (!allowances.empty()) {
      const auto share_exhausted = [&](std::string_view path) -> bool {
        std::string ancestor(path);
        while (true) {
          const auto share = allowances.find(ancestor);
          if (share != allowances.end() &&
              (share->second < 1 ||
               subtree_attempts(ancestor) >= share->second))
            return true;
          const std::size_t dash = ancestor.rfind('-');
          if (dash == std::string::npos) break;
          ancestor = ancestor.substr(0, dash);
        }
        return false;
      };
      for (auto it = tickets.begin(); it != tickets.end();) {
        // Decision 0025: imported tickets are never leafed by allowance;
        // if the budget cannot pay them they stay OPEN (0010 liveness).
        if (imported_paths.count(it->path)) {
          ++it;
          continue;
        }
        if (share_exhausted(it->path)) {
          writer.submit(state::CommitTerminal{{make_record(
              "SETTLE", {{"path", it->path},
                         {"kept", 0},
                         {"atomic", 0},
                         {"attempts", 0},
                         {"score", "0"},
                         {"end", "allowance spent"},
                         {"campaign", spec.id}})}});
          writer.sync();
          settled_samples.push_back(SettledSample{0, 0});
          result.leaves++;  // the allowance leaf counts like cannot-split
          ++impl_->zero_attempt_settles;
          it = tickets.erase(it);
          continue;
        }
        ++it;
      }
    }
    // The leaf-out above can settle dozens of zero-call tickets in one
    // pass; the guard must fire before the drained check short-circuits
    // the loop (decision 0025).
    if (impl_->zero_attempt_settles >= 50) {
      writer.submit(state::CommitTerminal{{make_record(
          "CAMPAIGN", {{"id", spec.id},
                       {"spec", campaign_spec_json(spec)},
                       {"state", "paused"},
                       {"reason", "no progress"}})}});
      writer.sync();
      result.end_reason = "paused";
      break;
    }
    if (tickets.empty() && waiting.empty()) {
      // No work here. In fleet mode the campaign is over once the fleet
      // budget is exhausted (the synced RESERVE records say so); otherwise
      // the device waits for the next sync round to bring lane work.
      if (!live_lane_members().empty() &&
          writer.reserved_total(spec.id) >= spec.loop_budget) {
        result.end_reason = "budget";
      } else {
        result.end_reason = "drained";
      }
      break;
    }

    // A spent chunk asks for the next one, only now that work exists.
    if (used_in_chunk >= current_chunk_size_) {
      const std::int64_t reserved = writer.reserved_total(spec.id);
      const bool fleet = !live_lane_members().empty();
      // Round state is shared with the sync scheduler thread
      // (note_sync_round): copy it out under the mutex, mutate it back
      // under the mutex, and keep every lock section I/O-free.
      bool round_complete = true;
      std::int64_t window_attempts = 0;
      {
        std::lock_guard pacing(impl_->pacing_mutex_);
        auto [it, inserted] = impl_->round_states.try_emplace(spec.id);
        if (inserted) {
          // A fresh gate (restart or a brand-new campaign) opens only
          // for a one-member fleet — 0/1 is a majority (0014 amendment).
          it->second.sync_round_complete =
              (live_lane_members().size() <= 1);
        }
        round_complete = it->second.sync_round_complete;
        window_attempts = it->second.last_window_attempts;
      }
      if (fleet && !round_complete) {
        // Decision 0014: at most one reservation per sync round. The node
        // loop re-enters run() after the next completed round.
        result.end_reason = "drained";
        break;
      }
      std::int64_t take;
      if (fleet) {
        // The measured chunk: max(50, attempts in the previous sync-round
        // window × 1.2, ceiling) — fast devices are not throttled.
        const std::int64_t measured = std::max<std::int64_t>(
            50, (window_attempts * 6 + 4) / 5);
        take = std::min(measured, spec.loop_budget - reserved);
      } else {
        take = std::min(spec.chunk, spec.loop_budget - reserved);
      }
      if (take <= 0) {
        result.end_reason = "budget";
        break;
      }
      current_chunk_size_ = take;
      used_in_chunk = 0;
      {
        std::lock_guard pacing(impl_->pacing_mutex_);
        impl_->round_states[spec.id].sync_round_complete = false;
      }
      log::Record reserve = make_record(
          "RESERVE", {{"campaign", spec.id}, {"attempts", take}});
      // The engine stamps the hlc so it can recognise its own entries for
      // the void check (decision 0014).
      reserve.hlc = impl_->hlc.now();
      impl_->own_reserve_hlcs[spec.id].push_back(reserve.hlc);
      writer.submit(state::CommitTerminal{{std::move(reserve)}});
      // The void check reads this reservation from the fold; a RESERVE is
      // once per sync round, so the sync is cheap.
      writer.sync();
    }

    (void)0;

    // A waiting ticket keeps its place in line (G9).
    Ticket ticket;
    bool from_waiting = false;
    // The speed factor excludes stolen work (decision 0014's measured
    // chunk counts own-lane attempts only).
    const bool ticket_stolen = stole_this_round;
    if (!waiting.empty()) {
      ticket = waiting.front();
      from_waiting = true;
    } else if (!tickets.empty()) {
      ticket = tickets.front();
    } else {
      // Nothing runnable right now; nothing will change without budget.
      result.end_reason = "budget";
      break;
    }

    // Device-budget reservation: a ticket that does not fit waits.
    const auto reservation =
        impl_->options.budget.reserve(impl_->options.ticket_worst_case_usd);
    if (!reservation) {
      if (from_waiting) {
        // Already waiting; with one worker nothing can release it.
        result.end_reason = "budget";
        break;
      }
      waiting.push_back(ticket);
      continue;  // try the next open ticket
    }
    if (from_waiting) waiting.pop_front();

    std::string error;
    const auto node = writer.db().node_by_path(ticket.path, &error);
    if (!node) {
      impl_->options.budget.settle(*reservation, "0");
      continue;
    }
    // A domain profile's levels.max is a real depth limit: a node already at
    // the deepest level is closed without a call (the planning profile
    // declares none, so its tree keeps growing as before).
    if (profile.max_level >= 0 && ticket.layer >= profile.max_level) {
      writer.submit(state::CommitTerminal{{make_record(
          "SETTLE", {{"path", ticket.path},
                     {"kept", 0},
                     {"atomic", 0},
                     {"attempts", 0},
                     {"score", "0"},
                     {"end", "profile depth limit"},
                     {"campaign", spec.id}})}});
      writer.sync();
      impl_->options.budget.settle(*reservation, "0");
      result.leaves++;
      continue;
    }
    // Profile semantics override generic campaign fallbacks per ticket.
    gate::FunnelOptions ticket_options = funnel_options;
    const int light_funnel_from =
        profile.light_funnel_from.value_or(spec.light_funnel_from);
    ticket_options.light = ticket.layer >= light_funnel_from;
    if (const auto range =
            onion::domain::child_range_for_parent(profile, ticket.layer)) {
      ticket_options.params.children_min = range->min;
      ticket_options.params.fallback_min = range->min;
      ticket_options.synth_target_min = range->min;
      ticket_options.synth_target_max = range->max;
      ticket_options.synth_max_children = range->max;
    }
    if (profile.atomic_verify) ticket_options.verify_atomic = *profile.atomic_verify;
    gate::Funnel funnel(ticket_options);
    const gate::NodeContext context = build_context(writer, *node, &error);
    gate::FunnelResult funnel_result = funnel.run(context, impl_->options.ask);
    result.calls += funnel_result.calls;
    bool provider_down = false;
    if (funnel_result.decision != gate::Decision::Defer)
      impl_->provider_failures = 0;

    for (const gate::Attempt& attempt : funnel_result.attempts) {
      writer.submit(state::AppendAttempt{make_record(
          "ATTEMPT", {{"path", ticket.path},
                      {"n", attempt.number},
                      {"score", std::to_string(attempt.score)},
                      {"passed", attempt.checks_passed ? 1 : 0},
                      {"tokens_in", attempt.tokens_in},
                      {"tokens_cached", attempt.tokens_cached},
                      {"tokens_out", attempt.tokens_out},
                      {"cost", attempt.cost_usd},
                      {"campaign", spec.id},
                      {"issues", attempt.issues}})});
      result.attempts++;
      if (!ticket_stolen) {
        // Shared with note_sync_round(): short, I/O-free section.
        std::lock_guard pacing(impl_->pacing_mutex_);
        impl_->round_states[spec.id].attempts_in_window++;
      }
    }

    // A deferred node consumed no attempt of the chunk (nothing was learned).
    if (funnel_result.decision != gate::Decision::Defer) used_in_chunk++;
    // Children are stored with the review so accept can settle without
    // spending more (decision 0008). The caches must update too: an open
    // review filters the path out of the ticket stream, and the children
    // join node_cache so an accept can expand them without a re-fetch.
    const auto emit_needs_review = [&](const gate::Attempt& chosen,
                                       std::string reason) {
      std::vector<log::Record> records;
      std::vector<std::string> new_ids;
      const bool ordered = onion::domain::ordered_children_for_parent(
          profile, ticket.layer);
      for (std::size_t i = 0; i < chosen.children.size(); ++i) {
        const gate::Child& child = chosen.children[i];
        const std::string id = node_ids_->allocate();
        new_ids.push_back(id);
        records.push_back(make_record(
            "ADD_NODE", {{"path", ticket.path + "-" + id},
                         {"layer", ticket.layer + 1},
                         {"title", child.title},
                         {"source", "engine"}}));
        if (!spec.tree.empty()) records.back().payload["tree"] = spec.tree;
        if (ordered)
          records.back().payload["sibling_order"] = static_cast<std::int64_t>(i);
      }
      records.push_back(make_record(
          "CRITIQUE", {{"path", ticket.path},
                       {"score", std::to_string(chosen.score)},
                       {"issues", attempt_issues(chosen)}}));
      records.push_back(make_record(
          "NEEDS_REVIEW", {{"path", ticket.path},
                           {"reason", std::move(reason)},
                           {"attempts", funnel_result.attempts.size()},
                           {"score", std::to_string(chosen.score)}}));
      writer.submit(state::CommitTerminal{std::move(records)});
      if (cache_valid) {
        review_cache.push_back(ticket.path);
        for (std::size_t i = 0; i < chosen.children.size(); ++i) {
          store::Node child_node;
          child_node.path = ticket.path + "-" + new_ids[i];
          child_node.layer = ticket.layer + 1;
          child_node.title = chosen.children[i].title;
          if (ordered) child_node.sibling_order = static_cast<int>(i);
          node_cache.push_back(std::move(child_node));
        }
      }
      result.reviews++;
    };
    switch (funnel_result.decision) {
      case gate::Decision::Leaf: {
        writer.submit(state::CommitTerminal{{make_record(
            "SETTLE", {{"path", ticket.path},
                       {"kept", 0},
                       {"atomic", 0},
                       {"attempts", funnel_result.attempts.size()},
                       {"score", "0"},
                       {"end", "cannot split"},
                       {"campaign", spec.id}})}});
        writer.sync();  // the pool fold reads durable attempts
        result.leaves++;
        // The guard counts settles with zero LLM calls: a cannot-split
        // leaf still ran the pre-check (a call), so it is not churn.
        if (funnel_result.calls == 0)
          ++impl_->zero_attempt_settles;
        else
          impl_->zero_attempt_settles = 0;
        break;
      }
      case gate::Decision::Commit: {
        const auto& chosen = funnel_result.attempts[funnel_result.chosen];
        // Structure (DESIGN › Campaigns › structure): expansion at the
        // profile's structure_layers runs only when the campaign allows
        // it; otherwise the result goes to review. The embedded planning
        // profile lists layers 0..2 — the historical L1..L3 rule.
        const bool needs_structure = [&] {
          for (const int layer : profile.structure_layers)
            if (layer == ticket.layer) return true;
          return false;
        }();
        if (spec.review_all || (!spec.structure && needs_structure)) {
          emit_needs_review(chosen, spec.review_all ? "campaign review all"
                                                     : "structure disallowed");
          break;
        }
        int atomic = 0;
        for (const gate::Child& child : chosen.children)
          if (child.atomic) atomic++;
        std::vector<log::Record> records;
        std::vector<std::string> new_ids;
        const bool ordered = onion::domain::ordered_children_for_parent(
            profile, ticket.layer);
        for (std::size_t i = 0; i < chosen.children.size(); ++i) {
          const gate::Child& child = chosen.children[i];
          const std::string id = node_ids_->allocate();
          new_ids.push_back(id);
          records.push_back(make_record(
              "ADD_NODE", {{"path", ticket.path + "-" + id},
                           {"layer", ticket.layer + 1},
                           {"title", child.title}}));
          if (!spec.tree.empty()) records.back().payload["tree"] = spec.tree;
          if (ordered)
            records.back().payload["sibling_order"] = static_cast<std::int64_t>(i);
          if (child.atomic)
            records.push_back(make_record(
                "ATOMIC", {{"path", ticket.path + "-" + id},
                           {"verified", true}}));
        }
        // The critique is recorded only when the campaign turned the critic
        // on (decision 0024); it never gated this commit.
        if (spec.critic)
          records.push_back(make_record(
              "CRITIQUE", {{"path", ticket.path},
                           {"score", std::to_string(chosen.score)},
                           {"issues", attempt_issues(chosen)}}));
        nlohmann::json settle_payload{{"path", ticket.path},
                            {"kept", chosen.children.size()},
                            {"atomic", atomic},
                            {"attempts", funnel_result.attempts.size()},
                            {"score", std::to_string(chosen.score)},
                            {"end", "children"},
                            {"campaign", spec.id}};
        if (funnel_result.below_target) settle_payload["below_target"] = true;
        records.push_back(make_record("SETTLE", std::move(settle_payload)));
        writer.submit(state::CommitTerminal{std::move(records)});
        // Fold the settle before the next refresh: the pool ledger
        // (decision 0020) reads the attempts table, and the parent's
        // pending attempts must be durable before its children's
        // allowances are computed.
        writer.sync();
        if (cache_valid) {
          for (std::size_t i = 0; i < chosen.children.size(); ++i) {
            store::Node child_node;
            child_node.path = ticket.path + "-" + new_ids[i];
            child_node.layer = ticket.layer + 1;
            child_node.title = chosen.children[i].title;
            if (ordered) child_node.sibling_order = static_cast<int>(i);
            node_cache.push_back(std::move(child_node));
          }
        }
        result.settled++;
        result.atomic += atomic;
        impl_->zero_attempt_settles = 0;  // real work happened
        // DESIGN › Quality gate: a child labelled atomic is a leaf only
        // after the separate verification agrees; unverified labels must
        // not make leaves.  The stop rule's atomic clause is the one
        // mechanism that would let them — only VERIFIED labels count
        // toward it (verify_atomic is off by default in M3, so the
        // model's raw labels score 0 here; DR 0021 covers batching).
        settled_samples.push_back(SettledSample{
            static_cast<int>(chosen.children.size()),
            funnel_options.verify_atomic ? atomic : 0});
        break;
      }
      case gate::Decision::Defer: {
        // F-GATE-2: the provider failed, so nothing is settled — the node
        // stays an open ticket and a later run works it again. A leaf here
        // would tombstone the whole subtree for a network blip.
        impl_->provider_failures++;
        const std::chrono::milliseconds delay = std::min(
            impl_->options.provider_backoff_max,
            std::chrono::milliseconds(
                impl_->options.provider_backoff_initial.count() *
                (std::int64_t{1}
                 << std::min(impl_->provider_failures - 1, 20))));
        impl_->provider_backoff_until =
            std::chrono::steady_clock::now() + delay;
        std::fprintf(stderr,
                     "provider unavailable (%s): %s deferred, retry in %lld "
                     "ms\n",
                     funnel_result.provider_failure.c_str(),
                     ticket.path.c_str(),
                     static_cast<long long>(delay.count()));
        provider_down = true;
        break;
      }
      case gate::Decision::Retry:
        break;  // unreachable: the funnel resolves internally
    }
    writer.sync();
    // Settle the reservation with what the ticket really cost (F-BUD-1).
    std::string actual_cost = "0";
    for (const gate::Attempt& attempt : funnel_result.attempts)
      actual_cost = gate::add_usd(actual_cost, attempt.cost_usd);
    impl_->options.budget.settle(*reservation, actual_cost);
    if (provider_down) {
      result.end_reason = "provider unavailable";
      break;
    }
  }

  // Only a real finish closes the campaign record (decision 0010): the
  // stop rule, or a drain in single-device mode. Exhaustion ("budget")
  // never closes it — open tickets resume if the budget is raised, and a
  // drained fleet lane waits for sync.
  const bool fleet = !live_lane_members().empty();
  if (result.end_reason == "stop rule" ||
      (result.end_reason == "drained" && !fleet)) {
    // The fold replaces the stored spec with this record's, so a domain
    // campaign (a named tree, a non-planning profile or review_all) must
    // restate its whole spec or the stop would erase its tree binding. A
    // planning campaign keeps its historical seven-key stop record, so its
    // records stay byte-identical (decision 0029).
    const bool domain_campaign =
        !spec.tree.empty() || spec.review_all ||
        (!spec.profile_hash.empty() &&
         spec.profile_hash !=
             onion::domain::profile_hash(onion::domain::planning_profile()));
    const nlohmann::json stop_spec =
        domain_campaign
            ? campaign_spec_json(spec)
            : nlohmann::json{{"scope", spec.scope},
                             {"loop_budget", spec.loop_budget},
                             {"mode", spec.mode},
                             {"gate", std::to_string(spec.gate)},
                             {"floor", std::to_string(spec.floor)},
                             {"chunk", spec.chunk},
                             {"model", spec.model}};
    writer.submit(state::CommitTerminal{{make_record(
        "CAMPAIGN", {{"id", spec.id},
                     {"spec", stop_spec},
                     {"state", "stopped"}})}});
    writer.sync();
  }
  return result;
}

void Engine::review_accept(std::string_view path, const CampaignSpec& spec) {
  std::string error;
  // M8 depth checkpoint: the node was paused WITHOUT an attempt, so it
  // has no children to settle — accepting releases it back into the
  // ticket stream (REVIEW decide only; the node stays unsettled and the
  // next run attempts it).
  for (const store::Review& review : impl_->options.writer.db().open_reviews(&error))
    if (review.path == path && review.reason == "depth checkpoint") {
      std::vector<log::Record> records;
      records.push_back(make_record(
          "REVIEW", {{"path", std::string(path)}, {"decision", "accept"},
                     {"note", ""}}));
      impl_->options.writer.submit(state::CommitTerminal{std::move(records)});
      impl_->options.writer.sync();
      return;
    }
  std::vector<store::Node> children =
      impl_->options.writer.db().nodes_with_path_prefix(path, &error);
  int kept = 0;
  int atomic = 0;
  for (const store::Node& child : children)
    if (parent_path(child.path) == path) kept++;
  std::vector<log::Record> records;
  records.push_back(make_record(
      "REVIEW", {{"path", std::string(path)}, {"decision", "accept"}, {"note", ""}}));
  records.push_back(make_record(
      "SETTLE", {{"path", std::string(path)},
                 {"kept", kept},
                 {"atomic", atomic},
                 {"attempts", 0},
                 {"score", "0"},
                 {"end", "children"},
                 {"campaign", spec.id}}));
  impl_->options.writer.submit(state::CommitTerminal{std::move(records)});
  impl_->options.writer.sync();
}

void Engine::review_retry(std::string_view path, const CampaignSpec& spec,
                          std::string note) {
  // M8 depth checkpoint: retry releases the paused ticket WITHOUT a
  // re-expansion lock (nothing was expanded; a dangling lock would
  // pause the children the retried attempt is about to create).
  std::string error;
  for (const store::Review& review : impl_->options.writer.db().open_reviews(&error))
    if (review.path == path && review.reason == "depth checkpoint") {
      std::vector<log::Record> records;
      records.push_back(make_record(
          "REVIEW", {{"path", std::string(path)}, {"decision", "retry"},
                     {"note", note}}));
      impl_->options.writer.submit(state::CommitTerminal{std::move(records)});
      impl_->options.writer.sync();
      return;
    }
  std::vector<log::Record> records;
  records.push_back(make_record(
      "REVIEW", {{"path", std::string(path)}, {"decision", "retry"}, {"note", note}}));
  records.push_back(make_record(
      "REEXPAND", {{"path", std::string(path)}, {"reason", note}}));
  impl_->options.writer.submit(state::CommitTerminal{std::move(records)});
  impl_->options.writer.sync();
}

void Engine::review_reject(std::string_view path, const CampaignSpec& spec) {
  std::string error;
  std::vector<store::Node> subtree =
      impl_->options.writer.db().nodes_with_path_prefix(path, &error);
  std::vector<log::Record> records;
  for (const store::Node& node : subtree)
    records.push_back(make_record(
        "TOMBSTONE", {{"path", node.path}, {"reason", "review rejected"}}));
  records.push_back(make_record(
      "REVIEW", {{"path", std::string(path)}, {"decision", "reject"}, {"note", ""}}));
  impl_->options.writer.submit(state::CommitTerminal{std::move(records)});
  impl_->options.writer.sync();
}


// ------------------------------------------------------------------ lanes

double hrw_score(std::string_view path, std::string_view device_id,
                 double weight) {
  const std::string combined = std::string(path) + std::string(device_id);
  const std::string digest = onion::log::blake2b_hex(combined);
  // First 64 bits big-endian from the first 16 hex characters.
  std::uint64_t x = 0;
  for (int i = 0; i < 16; ++i) {
    const char c = digest[static_cast<std::size_t>(i)];
    const int nibble = c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
    x = (x << 4) | static_cast<std::uint64_t>(nibble);
  }
  const double h = (static_cast<double>(x) + 0.5) / 18446744073709551616.0;
  // h is strictly in (0, 1), so ln(h) is finite and negative.
  return -weight / std::log(h);
}

std::optional<std::string> lane_owner(std::string_view path,
                                      const std::vector<LaneMember>& members) {
  if (members.empty()) return std::nullopt;
  double best_score = -std::numeric_limits<double>::infinity();
  std::string best_id;
  for (const LaneMember& member : members) {
    const double score = hrw_score(path, member.device_id, member.weight);
    if (score > best_score ||
        (score == best_score && member.device_id < best_id)) {
      best_score = score;
      best_id = member.device_id;
    }
  }
  return best_id;
}

// ----------------------------------------------------------------- plan

namespace {
double money(std::int64_t tokens, double per_million) {
  return static_cast<double>(tokens) * per_million / 1000000.0;
}

std::int64_t projected_log_growth(std::int64_t expanded_nodes,
                                  double bytes_per_node) {
  if (expanded_nodes <= 0 || !(bytes_per_node > 0.0)) return 0;
  const long double estimate = static_cast<long double>(expanded_nodes) *
                               static_cast<long double>(bytes_per_node);
  if (estimate >=
      static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(std::llround(estimate));
}
}  // namespace

CampaignPlan plan_campaign(const std::vector<std::int64_t>& level_nodes,
                           int target_depth, const PlanParams& params,
                           bool expand_children) {
  CampaignPlan plan;
  std::vector<std::int64_t> expandable = level_nodes;
  if (expandable.size() < static_cast<std::size_t>(target_depth + 2))
    expandable.resize(target_depth + 2, 0);

  const double per_attempt_in =
      params.calls_per_attempt * params.tokens_in_per_call;
  const double per_attempt_out =
      params.calls_per_attempt * params.tokens_out_per_call;
  const double hit = params.cache_hit_ratio;
  const double growth = params.children_kept * (1.0 - params.atomic_share);

  // Saturating helpers: a projection whose per-level growth exceeds int64
  // (the measured W5 growth does at depth 8+) clamps at INT64_MAX instead
  // of wrapping negative; the report notes the saturation.
  const auto sat = [](std::int64_t value) {
    return value < 0 ? std::numeric_limits<std::int64_t>::max() : value;
  };
  const auto sat_add = [&](std::int64_t a, std::int64_t b) {
    if (a >= std::numeric_limits<std::int64_t>::max() - b)
      return std::numeric_limits<std::int64_t>::max();
    return a + b;
  };

  for (int level = 0; level <= target_depth; ++level) {
    LevelPlan row;
    row.level = level;
    row.expandable = expandable[level];
    const double attempts_d =
        static_cast<double>(row.expandable) * params.attempts_avg;
    row.attempts =
        sat(static_cast<std::int64_t>(std::llround(attempts_d)));
    const double calls_d = attempts_d * params.calls_per_attempt +
        static_cast<double>(row.expandable) * params.precheck_calls;
    row.calls = sat(static_cast<std::int64_t>(std::llround(calls_d)));
    const double prechecks =
        static_cast<double>(row.expandable) * params.precheck_calls;
    const double in_d = attempts_d * per_attempt_in +
        prechecks * params.precheck_tokens_in;
    const double out_d = attempts_d * per_attempt_out +
        prechecks * params.precheck_tokens_out;
    row.tokens_in_hit = sat(static_cast<std::int64_t>(std::llround(in_d * hit)));
    row.tokens_in_miss = sat(static_cast<std::int64_t>(std::llround(in_d * (1.0 - hit))));
    row.tokens_out = sat(static_cast<std::int64_t>(std::llround(out_d)));
    row.new_nodes = sat(
        static_cast<std::int64_t>(std::llround(attempts_d / params.attempts_avg * growth)));
    if (expand_children && level + 1 < static_cast<int>(expandable.size()))
      expandable[level + 1] =
          sat_add(expandable[level + 1], row.new_nodes);
    const double cost_in_peak = money(row.tokens_in_miss, params.price_peak_in) +
        money(row.tokens_in_hit, params.price_peak_cache);
    const double cost_in_off = money(row.tokens_in_miss, params.price_offpeak_in) +
        money(row.tokens_in_hit, params.price_offpeak_cache);
    row.cost_peak = cost_in_peak + money(row.tokens_out, params.price_peak_out);
    row.cost_offpeak = cost_in_off + money(row.tokens_out, params.price_offpeak_out);
    row.hours = static_cast<double>(row.tokens_out) / params.output_tokens_per_s / 3600.0;
    plan.levels.push_back(row);

    plan.total_new_nodes = sat_add(plan.total_new_nodes, row.new_nodes);
    plan.total_expanded_nodes =
        sat_add(plan.total_expanded_nodes, row.expandable);
    plan.total_attempts = sat_add(plan.total_attempts, row.attempts);
    plan.total_calls = sat_add(plan.total_calls, row.calls);
    plan.tokens_in_miss = sat_add(plan.tokens_in_miss, row.tokens_in_miss);
    plan.tokens_in_hit = sat_add(plan.tokens_in_hit, row.tokens_in_hit);
    plan.tokens_out = sat_add(plan.tokens_out, row.tokens_out);
    if (row.new_nodes >= std::numeric_limits<std::int64_t>::max() ||
        row.tokens_out >= std::numeric_limits<std::int64_t>::max())
      plan.saturated = true;
    plan.cost_peak += row.cost_peak;
    plan.cost_offpeak += row.cost_offpeak;
    plan.hours += row.hours;
  }
  plan.spend_per_hour_peak = plan.hours > 0.0 ? plan.cost_peak / plan.hours : 0.0;
  plan.spend_per_hour_offpeak =
      plan.hours > 0.0 ? plan.cost_offpeak / plan.hours : 0.0;
  plan.expected_log_growth_bytes =
      projected_log_growth(plan.total_expanded_nodes, params.log_bytes_per_node);
  return plan;
}

CampaignPlan plan_grow_levels(std::int64_t expandable_total, int levels,
                              const PlanParams& params) {
  CampaignPlan plan;
  const double per_attempt_in =
      params.calls_per_attempt * params.tokens_in_per_call;
  const double per_attempt_out =
      params.calls_per_attempt * params.tokens_out_per_call;
  const double hit = params.cache_hit_ratio;
  const double growth = params.children_kept * (1.0 - params.atomic_share);

  const auto sat = [](std::int64_t value) {
    return value < 0 ? std::numeric_limits<std::int64_t>::max() : value;
  };
  const auto sat_add = [&](std::int64_t a, std::int64_t b) {
    if (a >= std::numeric_limits<std::int64_t>::max() - b)
      return std::numeric_limits<std::int64_t>::max();
    return a + b;
  };

  long double expandable = static_cast<long double>(expandable_total);
  for (int wave = 1; wave <= levels; ++wave) {
    LevelPlan row;
    row.level = wave;
    row.expandable = static_cast<std::int64_t>(expandable);
    const double attempts_d =
        static_cast<double>(expandable) * params.attempts_avg;
    row.attempts = sat(static_cast<std::int64_t>(std::llround(attempts_d)));
    const double calls_d = attempts_d * params.calls_per_attempt +
        static_cast<double>(expandable) * params.precheck_calls;
    row.calls = sat(static_cast<std::int64_t>(std::llround(calls_d)));
    const double prechecks =
        static_cast<double>(expandable) * params.precheck_calls;
    const double in_d = attempts_d * per_attempt_in +
        prechecks * params.precheck_tokens_in;
    const double out_d = attempts_d * per_attempt_out +
        prechecks * params.precheck_tokens_out;
    row.tokens_in_hit = sat(static_cast<std::int64_t>(std::llround(in_d * hit)));
    row.tokens_in_miss =
        sat(static_cast<std::int64_t>(std::llround(in_d * (1.0 - hit))));
    row.tokens_out = sat(static_cast<std::int64_t>(std::llround(out_d)));
    row.new_nodes = sat(static_cast<std::int64_t>(std::llround(
        static_cast<long double>(expandable) * growth)));
    row.cost_peak = money(row.tokens_in_miss, params.price_peak_in) +
        money(row.tokens_in_hit, params.price_peak_cache) +
        money(row.tokens_out, params.price_peak_out);
    row.cost_offpeak = money(row.tokens_in_miss, params.price_offpeak_in) +
        money(row.tokens_in_hit, params.price_offpeak_cache) +
        money(row.tokens_out, params.price_offpeak_out);
    row.hours = static_cast<double>(row.tokens_out) /
                params.output_tokens_per_s / 3600.0;
    plan.levels.push_back(row);

    plan.total_new_nodes = sat_add(plan.total_new_nodes, row.new_nodes);
    plan.total_expanded_nodes =
        sat_add(plan.total_expanded_nodes, row.expandable);
    plan.total_attempts = sat_add(plan.total_attempts, row.attempts);
    plan.total_calls = sat_add(plan.total_calls, row.calls);
    plan.tokens_in_miss = sat_add(plan.tokens_in_miss, row.tokens_in_miss);
    plan.tokens_in_hit = sat_add(plan.tokens_in_hit, row.tokens_in_hit);
    plan.tokens_out = sat_add(plan.tokens_out, row.tokens_out);
    plan.cost_peak += row.cost_peak;
    plan.cost_offpeak += row.cost_offpeak;
    plan.hours += row.hours;
    if (row.new_nodes >= std::numeric_limits<std::int64_t>::max())
      plan.saturated = true;
    expandable *= growth;
  }
  plan.spend_per_hour_peak =
      plan.hours > 0.0 ? plan.cost_peak / plan.hours : 0.0;
  plan.spend_per_hour_offpeak =
      plan.hours > 0.0 ? plan.cost_offpeak / plan.hours : 0.0;
  plan.expected_log_growth_bytes =
      projected_log_growth(plan.total_expanded_nodes, params.log_bytes_per_node);
  return plan;
}

PlanParams measure_params(const store::Db& db, std::string* error) {
  PlanParams params;
  std::string query_error;
  const auto settles = db.settles(&query_error);
  const auto attempts = db.attempts(&query_error);
  if (!query_error.empty() && error) *error = query_error;

  double kept_sum = 0.0;
  double atomic_sum = 0.0;
  double attempts_sum = 0.0;
  std::int64_t settles_with_children = 0;
  for (const auto& settle : settles) {
    if (settle.kept <= 0) continue;
    ++settles_with_children;
    kept_sum += static_cast<double>(settle.kept);
    atomic_sum += static_cast<double>(settle.atomic);
    attempts_sum += static_cast<double>(settle.attempts);
  }
  if (settles_with_children > 0) {
    params.children_kept = kept_sum / settles_with_children;
    params.atomic_share = atomic_sum / kept_sum;
    params.attempts_avg = attempts_sum / settles_with_children;
  }
  // Review-accepted settles record attempts 0 in the settle row even when
  // the funnel really ran attempts, so prefer the attempts table over the
  // settle rows when both exist: attempts_avg = attempt rows / settles.
  if (!attempts.empty() && !settles.empty())
    params.attempts_avg = static_cast<double>(attempts.size()) /
                          static_cast<double>(settles.size());
  double in_sum = 0.0;
  double out_sum = 0.0;
  std::int64_t attempt_rows = 0;
  for (const auto& attempt : attempts) {
    in_sum += static_cast<double>(attempt.tokens_in);
    out_sum += static_cast<double>(attempt.tokens_out);
    ++attempt_rows;
  }
  if (attempt_rows > 0) {
    // One attempt is about `calls_per_attempt` calls; recover the per-call
    // average from the measured per-attempt tokens.
    params.tokens_in_per_call = in_sum / attempt_rows / params.calls_per_attempt;
    params.tokens_out_per_call = out_sum / attempt_rows / params.calls_per_attempt;
  }
  // The observed footprint includes active raw segments and sealed zstd
  // segments, exactly what consumes device disk.  Settled nodes are the
  // completed units which produced those terminal batches, so this is the
  // measured bytes-per-node input for the next campaign's projection.
  const std::filesystem::path oplog = db.path().parent_path() / "oplog";
  std::error_code filesystem_error;
  std::uintmax_t log_bytes = 0;
  if (std::filesystem::exists(oplog, filesystem_error)) {
    for (std::filesystem::recursive_directory_iterator it(
             oplog, filesystem_error), end;
         !filesystem_error && it != end; it.increment(filesystem_error)) {
      if (!it->is_regular_file(filesystem_error)) continue;
      const std::uintmax_t size = it->file_size(filesystem_error);
      if (filesystem_error) break;
      if (size > std::numeric_limits<std::uintmax_t>::max() - log_bytes) {
        log_bytes = std::numeric_limits<std::uintmax_t>::max();
        break;
      }
      log_bytes += size;
    }
  }
  if (!settles.empty() && log_bytes > 0)
    params.log_bytes_per_node = static_cast<double>(log_bytes) /
                                static_cast<double>(settles.size());
  return params;
}

bool log_growth_matches_projection(std::int64_t observed_bytes,
                                   const CampaignPlan& plan,
                                   double tolerance) {
  if (observed_bytes < 0 || tolerance < 0.0) return false;
  if (plan.expected_log_growth_bytes == 0) return observed_bytes == 0;
  const long double projected =
      static_cast<long double>(plan.expected_log_growth_bytes);
  const long double delta = std::fabs(static_cast<long double>(observed_bytes) -
                                      projected);
  return delta <= projected * static_cast<long double>(tolerance);
}

std::string plan_report(const CampaignPlan& plan, const PlanParams& params) {
  std::ostringstream out;
  out << "level expandable new_nodes attempts calls in_miss in_hit out_tokens"
         " cost_peak cost_offpeak hours\n";
  for (const auto& row : plan.levels) {
    out << row.level << " " << row.expandable << " " << row.new_nodes << " "
        << row.attempts << " " << row.calls << " " << row.tokens_in_miss << " "
        << row.tokens_in_hit << " " << row.tokens_out << " "
        << std::fixed << std::setprecision(2) << row.cost_peak << " "
        << row.cost_offpeak << " " << row.hours << "\n";
  }
  out << "total new_nodes=" << plan.total_new_nodes
      << " attempts=" << plan.total_attempts
      << " calls=" << plan.total_calls << "\n";
  out << "tokens in_miss=" << plan.tokens_in_miss
      << " in_hit=" << plan.tokens_in_hit
      << " out=" << plan.tokens_out << "\n";
  out << std::fixed << std::setprecision(2)
      << "cost peak=" << plan.cost_peak << " offpeak=" << plan.cost_offpeak
      << " USD\n";
  out << "time at " << std::setprecision(0) << params.output_tokens_per_s
      << " output tokens/s: " << std::setprecision(2) << plan.hours << " hours\n";
  out << "spend/hour peak=" << plan.spend_per_hour_peak
      << " offpeak=" << plan.spend_per_hour_offpeak << " USD\n";
  out << "log growth=" << plan.expected_log_growth_bytes << " bytes ("
      << std::setprecision(2) << params.log_bytes_per_node
      << " bytes/settled node)\n";
  if (plan.saturated)
    out << "(saturated: values clamped at int64)\n";
  return out.str();
}

std::map<std::string, std::int64_t> compute_starting_pools(
    state::StateWriter& writer, const CampaignSpec& spec,
    std::string* error) {
  std::map<std::string, std::int64_t> pools;
  if (spec.scope.empty() || spec.loop_budget <= 0) return pools;
  // The same open-ticket rule the engine will work with (order_tickets):
  // under the scope, not settled, not a leaf, no open review above.
  std::vector<store::Node> nodes;
  for (const std::string& prefix : spec.scope)
    for (const store::Node& node :
         writer.db().nodes_with_path_prefix(prefix, error))
      nodes.push_back(node);
  std::vector<std::string> reviews;
  for (const store::Review& review : writer.db().open_reviews(error))
    if (review.state == "open" || review.state.empty())
      reviews.push_back(review.path);
  const std::vector<Ticket> tickets = order_tickets(
      nodes,
      [&](std::string_view path) { return writer.settled(path); },
      [&](std::string_view path) { return writer.leaf(path); },
      reviews, spec);
  // Count open tickets per root (the first path segment).
  std::map<std::string, std::int64_t> open;
  for (const Ticket& ticket : tickets) {
    const std::size_t dash = ticket.path.find('-');
    const std::string root =
        dash == std::string::npos ? ticket.path : ticket.path.substr(0, dash);
    ++open[root];
  }
  // A root with no tickets gets one attempt (decision 0020); the rest of
  // the budget splits proportionally to the ticket counts.
  for (const std::string& root : spec.scope)
    if (!open.count(root)) open[root] = 0;
  std::int64_t total_open = 0;
  for (const auto& [root, count] : open) total_open += count;
  if (total_open <= 0) {
    for (const std::string& root : spec.scope) pools[root] = 1;
    return pools;
  }
  std::int64_t handed_out = 0;
  for (auto it = open.begin(); it != open.end(); ++it) {
    const bool last = std::next(it) == open.end();
    const std::int64_t share =
        last
            ? spec.loop_budget - handed_out
            : (spec.loop_budget * it->second) / total_open;
    pools[it->first] = std::max<std::int64_t>(1, share);
    handed_out += pools[it->first];
  }
  return pools;
}

std::map<std::string, std::int64_t> fold_pool_allowances(
    const std::map<std::string, std::int64_t>& root_pools,
    const std::vector<PoolEvent>& events) {
  // The fold is order-independent by construction: events sort by
  // (hlc, origin), and the allowance of a child is frozen at its
  // creation from the parent's remaining pool AT THAT POINT.
  std::vector<const PoolEvent*> sorted;
  sorted.reserve(events.size());
  for (const PoolEvent& event : events) sorted.push_back(&event);
  std::sort(sorted.begin(), sorted.end(),
            [](const PoolEvent* a, const PoolEvent* b) {
              if (a->hlc != b->hlc) return a->hlc < b->hlc;
              return a->origin < b->origin;
            });

  std::map<std::string, std::int64_t> allowances;
  std::map<std::string, std::int64_t> spent;
  for (const auto& [root, pool] : root_pools) allowances[root] = pool;

  // remaining(node) = allowance(node) - spent in the node's SUBTREE.
  const auto subtree_spent = [&](std::string_view path) -> std::int64_t {
    std::int64_t total = 0;
    const std::string prefix = std::string(path) + "-";
    for (const auto& [node, count] : spent)
      if (node == path || node.rfind(prefix, 0) == 0) total += count;
    return total;
  };
  // Decision 0025: a node created BEFORE the campaign (an imported
  // ticket; the import stamps tiny hlcs) starts a FRESH chain at the
  // scope root's remaining pool — the 50%-halving applies only to
  // engine-created expansion children.
  const auto root_of = [&](std::string_view path) -> std::string {
    const std::size_t dash = path.find('-');
    return dash == std::string_view::npos ? std::string(path)
                                          : std::string(path.substr(0, dash));
  };

  for (const PoolEvent* event : sorted) {
    switch (event->kind) {
      case PoolEvent::Kind::Attempt:
        ++spent[event->path];
        break;
      case PoolEvent::Kind::Create: {
        // Imported node (decision 0025/0027): a fresh chain at the
        // scope root's remaining pool, never halved.
        if (event->source == "import") {
          const std::string root = root_of(event->path);
          const auto pool = root_pools.find(root);
          if (pool != root_pools.end())
            allowances[event->path] =
                std::max<std::int64_t>(0, pool->second -
                                              subtree_spent(root));
          else
            allowances[event->path] = 0;
          break;
        }
        // The parent: the nearest existing scoped ancestor (every node
        // in the ledger has one once its ancestors were created; a child
        // event always follows its ancestors' events in path depth —
        // fold order keeps the invariant when hlcs are sane; when they
        // are not, the parent lookup fails and the child keeps 0).
        std::string parent;
        {
          std::string candidate = event->path;
          while (true) {
            const std::size_t dash = candidate.rfind('-');
            if (dash == std::string::npos) break;
            candidate = candidate.substr(0, dash);
            if (allowances.count(candidate)) {
              parent = candidate;
              break;
            }
          }
        }
        if (parent.empty()) break;  // the root itself (or unknown): no halving
        const std::int64_t parent_allowance = allowances.at(parent);
        const std::int64_t remaining =
            std::max<std::int64_t>(0, parent_allowance -
                                      subtree_spent(parent));
        allowances[event->path] = remaining / 2;
        break;
      }
    }
  }
  return allowances;
}

}  // namespace onion::sched
