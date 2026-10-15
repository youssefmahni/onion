#include "api/fleet_json.hpp"

#include <map>
#include <set>

#include "store/store.hpp"

namespace onion::api::fleet_json {

nlohmann::json members(const store::Db& read,
                       const std::vector<MemberProbe>& probes) {
  std::string error;
  nlohmann::json result;

  std::map<std::string, MemberProbe> probe_by_id;
  for (const auto& probe : probes) probe_by_id[probe.device_id] = probe;

  auto statement = read.prepare(
      "SELECT device_id, slot, state, incarnation, sync_url, swim_url, "
      "profile_version, evict_cutoff FROM members ORDER BY slot;",
      &error);
  nlohmann::json items = nlohmann::json::array();
  if (!statement) {
    result["error"] = error.empty() ? "members unavailable" : error;
    return result;
  }
  std::string step_error;
  while (statement->step_row(&step_error)) {
    const std::string device_id = statement->text(0);
    nlohmann::json entry;
    entry["device_id"] = device_id;
    entry["slot"] = statement->integer(1);
    entry["state"] = statement->text(2);
    entry["incarnation"] = statement->integer(3);
    entry["sync_url"] = statement->text(4);
    entry["swim_url"] = statement->text(5);
    entry["profile_version"] = statement->integer(6);
    entry["evict_cutoff"] = statement->integer(7);
    const auto found = probe_by_id.find(device_id);
    entry["live_state"] =
        found != probe_by_id.end() ? nlohmann::json(found->second.state)
                                   : nlohmann::json(nullptr);
    entry["last_seen_s"] =
        found != probe_by_id.end() ? found->second.last_seen_s : -1;
    entry["lane_weight"] =
        found != probe_by_id.end() ? found->second.lane_weight : 0.0;
    items.push_back(std::move(entry));
  }
  if (!step_error.empty() && error.empty()) error = step_error;

  // Probe-only devices (e.g. this device before any join folds) appear
  // with their live data and empty table fields.
  std::set<std::string> seen;
  for (const auto& item : items) seen.insert(item["device_id"].get<std::string>());
  for (const auto& probe : probes) {
    if (seen.count(probe.device_id)) continue;
    items.push_back({{"device_id", probe.device_id},
                     {"slot", nullptr},
                     {"state", nullptr},
                     {"incarnation", nullptr},
                     {"sync_url", ""},
                     {"swim_url", ""},
                     {"profile_version", nullptr},
                     {"evict_cutoff", nullptr},
                     {"live_state", probe.state},
                     {"last_seen_s", probe.last_seen_s},
                     {"lane_weight", probe.lane_weight}});
  }
  result["items"] = std::move(items);
  if (!error.empty()) result["error"] = error;
  return result;
}

}  // namespace onion::api::fleet_json
