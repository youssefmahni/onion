#include "api/alerts.hpp"

#include <cstdlib>

namespace onion::api::alerts {

namespace {

nlohmann::json alert(std::string id, std::string severity,
                     std::string title, std::string detail,
                     nlohmann::json actions = nlohmann::json::array()) {
  return {{"id", std::move(id)},
          {"severity", std::move(severity)},
          {"title", std::move(title)},
          {"detail", std::move(detail)},
          {"actions", std::move(actions)}};
}

nlohmann::json action(std::string label, std::string method,
                      std::string path, bool confirm = false) {
  return {{"label", std::move(label)},
          {"method", std::move(method)},
          {"path", std::move(path)},
          {"confirm", confirm}};
}

double usd(std::string_view text) {
  return std::atof(std::string(text).c_str());
}

}  // namespace

nlohmann::json list(const Inputs& inputs) {
  nlohmann::json items = nlohmann::json::array();

  // Campaign paused by the churn guard (decision 0025: reason "no progress").
  for (const auto& campaign : inputs.campaigns) {
    if (campaign.state == "paused" && campaign.reason == "no progress")
      items.push_back(alert(
          "campaign-paused-" + campaign.id, "warning", "Campaign paused",
          "campaign " + campaign.id +
              " was paused by the no-progress guard",
          nlohmann::json::array(
              {action("Resume", "POST",
                      "/api/v1/campaigns/" + campaign.id + "/resume"),
               action("Inspect", "GET", "/api/v1/campaigns/" + campaign.id)})));
  }

  // Forked batches quarantined.
  if (inputs.forked_batches && *inputs.forked_batches > 0)
    items.push_back(alert(
        "forked-batches", "critical", "Forked batches quarantined",
        std::to_string(*inputs.forked_batches) +
            " incoming batch(es) failed chain verification and were not "
            "applied — another device may have a diverging log",
        nlohmann::json::array(
            {action("Inspect", "GET", "/api/state")})));

  // Budget exhausted (device).
  if (inputs.device_budget) {
    const double spend = usd(inputs.device_budget->spend_usd);
    const double budget = usd(inputs.device_budget->budget_usd);
    if (budget > 0 && spend >= budget)
      items.push_back(alert(
          "device-budget-exhausted", "critical", "Device budget exhausted",
          "spend " + inputs.device_budget->spend_usd + " USD of " +
              inputs.device_budget->budget_usd + " USD",
          nlohmann::json::array(
              {action("Raise budget", "POST", "/api/v1/device/budget")})));
  }

  // Suspect or dead members.
  for (const auto& [device, state] : inputs.member_states) {
    if (state == "SUSPECT" || state == "DEAD")
      items.push_back(alert(
          "member-" + state + "-" + device, "critical",
          "Member " + state,
          "device " + device + " is " + state,
          nlohmann::json::array({action("Inspect", "GET", "/api/v1/members")})));
  }

  // Sync lag: no completed round for more than 3 round intervals while
  // the fleet has more than one member.
  if (inputs.sync && inputs.sync->members > 1) {
    const std::int64_t limit = 3 * inputs.sync->round_interval_s;
    if (inputs.sync->seconds_since_last_round > limit)
      items.push_back(alert(
          "sync-lag", "warning", "Sync lag",
          std::to_string(inputs.sync->seconds_since_last_round) +
              " s since the last completed sync round (limit " +
              std::to_string(limit) + " s)",
          nlohmann::json::array({action("Inspect", "GET", "/api/v1/members")})));
  }

  // Disk below 10%.
  if (inputs.disk && inputs.disk->total_gb > 0 &&
      inputs.disk->free_gb / inputs.disk->total_gb < 0.10)
    items.push_back(alert(
        "disk-low", "critical", "Disk almost full",
        "free disk is " + std::to_string(static_cast<int>(inputs.disk->free_gb)) +
            " GB — below 10% of the volume",
        nlohmann::json::array()));

  // Review queue above its threshold.
  if (inputs.review_open && inputs.review_threshold &&
      *inputs.review_open > *inputs.review_threshold)
    items.push_back(alert(
        "review-backlog", "warning", "Review queue backing up",
        std::to_string(*inputs.review_open) + " open items (threshold " +
            std::to_string(*inputs.review_threshold) + ")",
        nlohmann::json::array(
            {action("Review", "GET", "/api/v1/reviews")})));

  // Circuit breaker open.
  if (inputs.breaker && inputs.breaker->open)
    items.push_back(alert(
        "circuit-breaker", "critical", "LLM circuit breaker open",
        inputs.breaker->reason.empty() ? "consecutive failures"
                                       : inputs.breaker->reason,
        nlohmann::json::array(
            {action("Inspect", "GET", "/api/v1/device/llm")})));

  return {{"items", std::move(items)}};
}

}  // namespace onion::api::alerts
