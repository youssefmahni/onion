#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace onion::state {
class StateWriter;
}
namespace onion::metrics {
class Store;
}

namespace onion::api::control_json {

// Live-number probes the node owns (engine/limiter/sync); the builders
// below only read them — the unit tests drive the shapes directly.
struct SummaryProbes {
  std::size_t inflight{};
  std::size_t limit{};
  std::string device_budget_usd;
  std::int64_t sync_rounds_completed{};
  std::int64_t seconds_since_last_round{};
  bool viewcache_ready{};
};

nlohmann::json summary(const state::StateWriter& writer,
                       const metrics::Store* metrics,
                       const SummaryProbes& probes);

// The speed series, downsampled server-side to at most max_points
// (bucketed means). range: 1h | 24h | 7d.
nlohmann::json speed_series(const metrics::Store& metrics,
                            std::string_view range, std::size_t max_points);

// The spend series (per campaign summed per hour).
nlohmann::json spend_series(const metrics::Store& metrics,
                            std::string_view range);

// The activity log after `since`, bounded to 200 items.
nlohmann::json events_since(const metrics::Store& metrics, std::int64_t since);

}  // namespace onion::api::control_json
