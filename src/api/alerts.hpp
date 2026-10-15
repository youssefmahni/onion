#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace onion::api::alerts {

// §4.4: alerts are derived server-side and cleared when the condition
// ends. Every input is an optional probe so each rule raises and clears
// independently (and the unit tests drive each one).
struct CampaignView {
  std::string id;
  std::string state;    // running | paused | stopped
  std::string reason;   // "no progress" -> the churn guard paused it
};

struct Inputs {
  std::vector<CampaignView> campaigns;
  std::optional<std::int64_t> forked_batches;   // quarantined forks
  std::optional<std::int64_t> review_open;      // open review items
  std::optional<std::int64_t> review_threshold{100};
  struct SpendBudget {
    std::string spend_usd;   // decimal strings
    std::string budget_usd;
  };
  std::optional<SpendBudget> device_budget;
  std::vector<std::pair<std::string, std::string>> member_states;
  struct Sync {
    std::size_t members{1};
    std::int64_t seconds_since_last_round{0};
    std::int64_t round_interval_s{15};
  };
  std::optional<Sync> sync;
  struct Disk {
    double free_gb{};
    double total_gb{};
  };
  std::optional<Disk> disk;
  struct Breaker {
    bool open{};
    std::string reason;
  };
  std::optional<Breaker> breaker;
};

nlohmann::json list(const Inputs& inputs);

}  // namespace onion::api::alerts
