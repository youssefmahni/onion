#include "sched.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <limits>
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
  spec.gate = value.value("gate", 0.7);
  spec.max_attempts = value.value("max_attempts", 3);
  spec.floor = value.value("floor", 0.5);
  spec.chunk = value.value("chunk", std::int64_t{50});
  spec.light_funnel_from = value.value("light_funnel_from", 8);
  spec.structure = value.value("structure", false);
  spec.model = value.value("model", std::string{"deepseek-flash"});
  spec.critic_model = value.value("critic_model", std::string{});
  spec.auto_accept = value.value("auto_accept", false);
  if (spec.loop_budget < 1) return fail("loop_budget must be positive");
  if (spec.chunk < 1) return fail("chunk must be positive");
  return spec;
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
                   std::size_t window) {
  if (settled_in_order.size() < window) return false;
  const std::size_t begin = settled_in_order.size() - window;
  int atomic = 0;
  double score_sum = 0.0;
  for (std::size_t i = begin; i < settled_in_order.size(); ++i) {
    atomic += settled_in_order[i].atomic;
    score_sum += settled_in_order[i].score;
  }
  return static_cast<double>(atomic) / static_cast<double>(window) >= 0.9 &&
         score_sum / static_cast<double>(window) >= 0.8;
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

struct Engine::Impl {
  Options options;
  std::atomic<bool> internal_stop{false};
};

Engine::Engine(Options options)
    : impl_(new Impl{std::move(options), {false}}) {}
Engine::~Engine() = default;

namespace {

log::Record make_record(std::string type, nlohmann::json payload) {
  log::Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  return record;
}

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

Engine::RunResult Engine::run(const CampaignSpec& spec,
                              std::atomic<bool>* external_stop) {
  RunResult result;
  auto& writer = impl_->options.writer;
  const auto existing = writer.campaign(spec.id);
  if (existing && existing->state != "running") {
    result.end_reason = existing->state;
    return result;
  }

  // Emit the CAMPAIGN record (start).
  writer.submit(state::CommitTerminal{{make_record(
      "CAMPAIGN", {{"id", spec.id},
                   {"spec", {{"scope", spec.scope},
                             {"loop_budget", spec.loop_budget},
                             {"mode", spec.mode},
                             // Records never contain floats (decision 0003):
                             // scores are exact decimal strings.
                             {"gate", std::to_string(spec.gate)},
                             {"floor", std::to_string(spec.floor)},
                             {"chunk", spec.chunk},
                             {"model", spec.model}}},
                   {"state", "running"}})}});

  gate::FunnelOptions funnel_options;
  funnel_options.params.pass_score = spec.gate;
  funnel_options.params.floor_score = spec.floor;
  funnel_options.params.max_attempts = spec.max_attempts;
  funnel_options.model = spec.critic_model.empty() ? spec.model
                                                   : spec.critic_model;
  funnel_options.prices = impl_->options.prices;

  std::vector<SettledSample> settled_samples;
  std::deque<Ticket> waiting;
  std::unique_ptr<NodeIdAllocator> node_ids_;
  std::int64_t reserved_attempts = 0;
  std::int64_t used_in_chunk = 0;

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
    return order_tickets(node_cache,
                         [&](std::string_view path) { return writer.settled(path); },
                         [&](std::string_view path) { return writer.leaf(path); },
                         review_cache, spec);
  };

  for (;;) {
    if (stop_requested()) {
      result.end_reason = "stopped";
      break;
    }
    if (stop_rule_met(settled_samples)) {
      result.end_reason = "stop rule";
      break;
    }
    if (reserved_attempts >= spec.loop_budget) {
      result.end_reason = "budget";
      break;
    }

    std::vector<Ticket> tickets = refresh_tickets();
    if (tickets.empty() && waiting.empty()) cache_valid = false;
    if (tickets.empty() && waiting.empty()) {
      result.end_reason = "drained";
      break;
    }

    // Loop-budget chunking: one RESERVE chunk at a time.
    if (used_in_chunk == 0) {
      const std::int64_t take = std::min(spec.chunk,
                                         spec.loop_budget - reserved_attempts);
      reserved_attempts += take;
      writer.submit(state::CommitTerminal{{make_record(
          "RESERVE", {{"campaign", spec.id}, {"attempts", take}})}});
    }

    // A waiting ticket keeps its place in line (G9).
    Ticket ticket;
    bool from_waiting = false;
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
    // The light funnel runs from the campaign's light-funnel depth.
    gate::FunnelOptions ticket_options = funnel_options;
    ticket_options.light = ticket.layer >= spec.light_funnel_from;
    gate::Funnel funnel(ticket_options);
    const gate::NodeContext context = build_context(writer, *node, &error);
    gate::FunnelResult funnel_result = funnel.run(context, impl_->options.ask);
    result.calls += funnel_result.calls;

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
    }

    used_in_chunk++;
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
        result.leaves++;
        settled_samples.push_back(SettledSample{0, 0.0});
        break;
      }
      case gate::Decision::Commit: {
        const auto& chosen = funnel_result.attempts[funnel_result.chosen];
        int atomic = 0;
        for (const gate::Child& child : chosen.children)
          if (child.atomic) atomic++;
        std::vector<log::Record> records;
        std::vector<std::string> new_ids;
        for (const gate::Child& child : chosen.children) {
          const std::string id = node_ids_->allocate();
          new_ids.push_back(id);
          records.push_back(make_record(
              "ADD_NODE", {{"path", ticket.path + "-" + id},
                           {"layer", ticket.layer + 1},
                           {"title", child.title}}));
          if (child.atomic)
            records.push_back(make_record(
                "ATOMIC", {{"path", ticket.path + "-" + id},
                           {"verified", true}}));
        }
        records.push_back(make_record(
            "CRITIQUE", {{"path", ticket.path},
                         {"score", std::to_string(chosen.score)},
                         {"issues", attempt_issues(chosen)}}));
        records.push_back(make_record(
            "SETTLE", {{"path", ticket.path},
                       {"kept", chosen.children.size()},
                       {"atomic", atomic},
                       {"attempts", funnel_result.attempts.size()},
                       {"score", std::to_string(chosen.score)},
                       {"end", "children"},
                       {"campaign", spec.id}}));
        writer.submit(state::CommitTerminal{std::move(records)});
        if (cache_valid) {
          for (std::size_t i = 0; i < chosen.children.size(); ++i) {
            store::Node child_node;
            child_node.path = ticket.path + "-" + new_ids[i];
            child_node.layer = ticket.layer + 1;
            child_node.title = chosen.children[i].title;
            node_cache.push_back(std::move(child_node));
          }
        }
        result.settled++;
        result.atomic += atomic;
        settled_samples.push_back(SettledSample{atomic > 0 ? 1 : 0,
                                                chosen.score});
        break;
      }
      case gate::Decision::NeedsReview: {
        const auto& chosen = funnel_result.attempts[funnel_result.chosen];
        // Children are stored with the review so accept can settle without
        // spending more (decision 0008).
        std::vector<log::Record> records;
        std::vector<std::string> new_ids;
        for (const gate::Child& child : chosen.children) {
          const std::string id = node_ids_->allocate();
          new_ids.push_back(id);
          records.push_back(make_record(
              "ADD_NODE", {{"path", ticket.path + "-" + id},
                           {"layer", ticket.layer + 1},
                           {"title", child.title}}));
        }
        records.push_back(make_record(
            "CRITIQUE", {{"path", ticket.path},
                         {"score", std::to_string(chosen.score)},
                         {"issues", attempt_issues(chosen)}}));
        records.push_back(make_record(
            "NEEDS_REVIEW", {{"path", ticket.path},
                             {"reason", "below floor"},
                             {"attempts", funnel_result.attempts.size()},
                             {"score", std::to_string(chosen.score)}}));
        writer.submit(state::CommitTerminal{std::move(records)});
        if (cache_valid) {
          review_cache.push_back(ticket.path);  // pause the subtree now
          for (std::size_t i = 0; i < chosen.children.size(); ++i) {
            store::Node child_node;
            child_node.path = ticket.path + "-" + new_ids[i];
            child_node.layer = ticket.layer + 1;
            child_node.title = chosen.children[i].title;
            node_cache.push_back(std::move(child_node));
          }
        }
        result.reviews++;
        break;
      }
      case gate::Decision::Retry:
        break;  // unreachable: the funnel resolves internally
    }
    writer.sync();
    impl_->options.budget.settle(*reservation, "0");
  }

  // A campaign that finished by itself (stop rule, drained, budget) is
  // closed: stop is final for the campaign record too.
  if (result.end_reason == "stop rule" || result.end_reason == "drained" ||
      result.end_reason == "budget") {
    writer.submit(state::CommitTerminal{{make_record(
        "CAMPAIGN", {{"id", spec.id},
                     {"spec", {{"scope", spec.scope},
                               {"loop_budget", spec.loop_budget},
                               {"mode", spec.mode},
                               {"gate", std::to_string(spec.gate)},
                               {"floor", std::to_string(spec.floor)},
                               {"chunk", spec.chunk},
                               {"model", spec.model}}},
                     {"state", "stopped"}})}});
    writer.sync();
  }
  return result;
}

void Engine::review_accept(std::string_view path, const CampaignSpec& spec) {
  std::string error;
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

}  // namespace onion::sched
