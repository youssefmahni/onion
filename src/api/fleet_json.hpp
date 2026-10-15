#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace onion::store {
class Db;
}

namespace onion::api::fleet_json {

// D5 members (plan §4.1). The node supplies live probes (SWIM state,
// lane weights); the provider merges them with the durable members table.

struct MemberProbe {
  std::string device_id;
  std::string state;             // ACTIVE | JOINING | SUSPECT | DEAD | ...
  std::int64_t last_seen_s{};    // -1 = unknown
  double lane_weight{};
};

nlohmann::json members(const store::Db& read,
                       const std::vector<MemberProbe>& probes);

}  // namespace onion::api::fleet_json
