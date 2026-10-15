#include "api/campaign_json.hpp"

#include <algorithm>
#include <map>

#include "sched/sched.hpp"
#include "state/state.hpp"
#include "store/store.hpp"

namespace onion::api::campaign_json {

namespace {

nlohmann::json campaign_entry(const state::StateWriter& writer,
                              const state::CampaignInfo& campaign) {
  std::string error;
  const store::Db& read = writer.read_db();
  nlohmann::json entry;
  entry["id"] = campaign.id;
  entry["state"] = campaign.state;
  entry["reason"] = campaign.reason;
  try {
    entry["spec"] = nlohmann::json::parse(campaign.spec);
  } catch (const std::exception&) {
    entry["spec"] = nlohmann::json::object();
  }
  entry["reserved"] = writer.reserved_total(campaign.id);

  // Budget used: the attempts' recorded costs for this campaign.
  auto spend_stmt = read.prepare(
      "SELECT COALESCE(SUM(CAST(cost AS REAL)), 0) FROM attempts WHERE "
      "campaign = ?1;",
      &error);
  double used = 0;
  if (spend_stmt) {
    std::string bind_error;
    spend_stmt->bind(1, campaign.id, &bind_error);
    std::string step_error;
    if (spend_stmt->step_row(&step_error)) used = spend_stmt->real(0);
  }
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.8f", used);
  std::string used_text(buffer);
  while (used_text.size() > 2 && used_text.back() == '0') used_text.pop_back();
  if (!used_text.empty() && used_text.back() == '.') used_text.pop_back();
  entry["budget_used_usd"] = used_text;

  // Settles by end reason.
  auto end_stmt = read.prepare(
      "SELECT end, COUNT(*) FROM settles WHERE campaign = ?1 GROUP BY end "
      "ORDER BY 2 DESC;",
      &error);
  nlohmann::json endings = nlohmann::json::array();
  if (end_stmt) {
    std::string bind_error;
    end_stmt->bind(1, campaign.id, &bind_error);
    std::string step_error;
    while (end_stmt->step_row(&step_error))
      endings.push_back(
          {{"end", end_stmt->text(0)}, {"n", end_stmt->integer(1)}});
  }
  entry["settles_by_end"] = std::move(endings);

  // Per-level progress: settled nodes per layer inside the campaign scope.
  auto level_stmt = read.prepare(
      "SELECT n.layer, COUNT(*) FROM settles s JOIN nodes n ON "
      "n.path = s.path WHERE s.campaign = ?1 GROUP BY n.layer ORDER BY "
      "n.layer;",
      &error);
  nlohmann::json levels = nlohmann::json::array();
  if (level_stmt) {
    std::string bind_error;
    level_stmt->bind(1, campaign.id, &bind_error);
    std::string step_error;
    while (level_stmt->step_row(&step_error))
      levels.push_back(
          {{"layer", level_stmt->integer_int(0)},
           {"settled", level_stmt->integer(1)}});
  }
  entry["settles_per_level"] = std::move(levels);
  return entry;
}

}  // namespace

nlohmann::json list(const state::StateWriter& writer) {
  nlohmann::json result;
  nlohmann::json items = nlohmann::json::array();
  for (const auto& campaign : writer.campaigns())
    items.push_back(campaign_entry(writer, campaign));
  result["items"] = std::move(items);
  return result;
}

nlohmann::json detail(const state::StateWriter& writer, std::string id) {
  const auto campaign = writer.campaign(id);
  if (!campaign) return {{"error", "not found"}};
  return campaign_entry(writer, *campaign);
}

nlohmann::json plan(const state::StateWriter& writer,
                    const nlohmann::json& spec_json) {
  std::string error;
  nlohmann::json result;
  std::string parse_error;
  const auto spec = onion::sched::parse_campaign(spec_json, &parse_error);
  if (!spec) {
    result["error"] = parse_error.empty() ? "invalid campaign spec"
                                          : parse_error;
    return result;
  }
  const onion::sched::PlanParams params =
      onion::sched::measure_params(writer.db(), &error);
  if (!error.empty()) {
    result["error"] = error;
    return result;
  }
  std::vector<std::int64_t> level_nodes;
  {
    std::string count_error;
    for (const auto& prefix : spec->scope)
      for (const auto& node :
           writer.db().nodes_with_path_prefix(prefix, &count_error)) {
        if (static_cast<std::size_t>(node.layer) >= level_nodes.size())
          level_nodes.resize(node.layer + 1, 0);
        ++level_nodes[node.layer];
      }
    if (level_nodes.empty()) level_nodes.push_back(1);
  }
  const int depth = spec_json.value("plan_depth", 8);
  const auto projected = onion::sched::plan_campaign(level_nodes, depth,
                                                     params);
  result["report"] = onion::sched::plan_report(projected, params);
  result["projected"] = {
      {"total_expanded_nodes", projected.total_expanded_nodes},
      {"total_attempts", projected.total_attempts},
      {"total_calls", projected.total_calls},
      {"tokens_in_miss", projected.tokens_in_miss},
      {"tokens_in_hit", projected.tokens_in_hit},
      {"tokens_out", projected.tokens_out},
      {"cost_peak", projected.cost_peak},
      {"cost_offpeak", projected.cost_offpeak},
      {"hours", projected.hours},
      {"spend_per_hour_peak", projected.spend_per_hour_peak},
      {"spend_per_hour_offpeak", projected.spend_per_hour_offpeak}};
  result["plan_depth"] = depth;
  return result;
}

}  // namespace onion::api::campaign_json
