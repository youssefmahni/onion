#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace onion::state {
class StateWriter;
}

namespace onion::api::campaign_json {

// D3 campaign reads (plan §4.1): list + detail over the writer's folded
// state. Projections reuse the sched plan model (decision 0016), exactly
// like the CLI's `campaign plan`.

nlohmann::json list(const state::StateWriter& writer);

nlohmann::json detail(const state::StateWriter& writer, std::string id);

// plan-before-start: parses the spec, measures the plan parameters from
// the store, and returns the projection (no writes). The caller mints the
// plan_id (valid 10 minutes) after a successful projection.
nlohmann::json plan(const state::StateWriter& writer,
                    const nlohmann::json& spec_json);

}  // namespace onion::api::campaign_json
