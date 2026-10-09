#include "api/api.hpp"
#include "cli/cli.hpp"
#include "core/core.hpp"
#include "gate/gate.hpp"
#include "gpu/gpu.hpp"
#include "llm/llm.hpp"
#include "log/log.hpp"
#include "platform/platform.hpp"
#include "profile/profile.hpp"
#include "sched/sched.hpp"
#include "state/state.hpp"
#include "store/store.hpp"
#include "swim/swim.hpp"
#include "sync/sync.hpp"

#include <array>
#include <string_view>

int main() {
  constexpr std::array<std::string_view, 14> components = {
      onion::api::component_name,
      onion::cli::component_name,
      onion::core::component_name,
      onion::gate::component_name,
      onion::gpu::component_name,
      onion::llm::component_name,
      onion::log::component_name,
      onion::platform::component_name,
      onion::profile::component_name,
      onion::sched::component_name,
      onion::state::component_name,
      onion::store::component_name,
      onion::swim::component_name,
      onion::sync::component_name,
  };

  for (const auto component : components) {
    if (component.empty()) {
      return 1;
    }
  }
  return 0;
}
