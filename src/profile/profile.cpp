#include "profile.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <thread>

#include "core/depot_core.hpp"
#include "gpu/depot_gpu.hpp"
#include "llm/llm.hpp"
#include "log/log.hpp"
#include "platform/platform.hpp"

namespace onion::profile {

Scan scan(const ScanOptions& options) {
  ScanInputs inputs;
  inputs.os = onion::platform::os_name();
  inputs.cores = static_cast<int>(std::thread::hardware_concurrency());
  if (inputs.cores <= 0) inputs.cores = 1;
  if (const auto ram = onion::platform::total_ram_gb()) inputs.ram_gb = *ram;
  else inputs.ram_gb = -1;
  if (const auto disk = onion::platform::disk_free_gb(options.data_dir))
    inputs.disk_free_gb = *disk;
  else inputs.disk_free_gb = -1;
  if (const auto link = onion::platform::link_speed_mbps())
    inputs.link_mbps = *link;
  else inputs.link_mbps = -1;

  // Durable-write latency: append + sync a probe file in the data directory.
  {
    std::error_code ec;
    std::filesystem::create_directories(options.data_dir, ec);
    const auto path = std::filesystem::path(options.data_dir) / ".durable_probe";
    const auto started = std::chrono::steady_clock::now();
    onion::platform::AppendFile file;
    if (file.open(path) && file.append("probe\n") && file.sync()) {
      inputs.durable_write_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - started)
              .count();
      std::filesystem::remove(path, ec);
    } else {
      inputs.durable_write_ms = -1;
    }
  }

  // GPU fp64 from the OpenCL layer.
  for (const auto& device : depot::gpu::list_devices())
    if (device.fp64) inputs.gpu_fp64 = true;

  // Battery and load from the platform sampler.
  const onion::platform::Sample sample = onion::platform::sample();
  inputs.on_battery = sample.on_battery;

  // Provider round-trip to /models.
  if (!options.provider_url.empty()) {
    const auto started = std::chrono::steady_clock::now();
    long status = 0;
    long version = 0;
    onion::llm::http_get(options.provider_url + "/models", options.api_key,
                         status, version);
    inputs.llm_rtt_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
  } else {
    inputs.llm_rtt_ms = -1;
  }
  return make_scan(inputs);
}

Scan make_scan(const ScanInputs& inputs) {
  Scan scan;
  scan.os = inputs.os;
  scan.cores = inputs.cores;
  scan.ram_gb = inputs.ram_gb;
  scan.disk_free_gb = inputs.disk_free_gb;
  scan.durable_write_ms = inputs.durable_write_ms;
  scan.gpu_fp64 = inputs.gpu_fp64;
  scan.on_battery = inputs.on_battery;
  scan.link_mbps = inputs.link_mbps;
  scan.llm_rtt_ms = inputs.llm_rtt_ms;
  return scan;
}

Rules rules_v1(bool laptop, int cores) {
  Rules rules;
  if (laptop) {
    rules.base_weight = 0.25;
    rules.start_limit = 4;
    rules.ceiling = 32;
    rules.cpu_pool = cores;
    rules.on_battery_cap = 8;
    rules.ram_floor_gb = 1.0;
  } else {
    rules.base_weight = 1.0;
    rules.start_limit = 16;
    rules.ceiling = 2000;
    rules.cpu_pool = cores;
    rules.on_battery_cap = -1;
    rules.ram_floor_gb = 2.0;
  }
  return rules;
}

std::map<std::string, std::string> parse_node_conf(const std::string& text) {
  std::map<std::string, std::string> values;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line = line.substr(0, comment);
    line = depot::trim(line);
    if (line.empty()) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = depot::trim(line.substr(0, equals));
    const std::string value = depot::trim(line.substr(equals + 1));
    if (!key.empty()) values[key] = value;
  }
  return values;
}

DerivedSettings derive(const Scan& scan,
                       const std::map<std::string, std::string>& overrides) {
  DerivedSettings settings;
  const bool laptop = scan.on_battery.has_value();
  settings.rules = rules_v1(laptop, scan.cores);
  settings.overrides = overrides;
  // node.conf overrides always win.
  const auto override_double = [&](const std::string& key,
                                   double& target) {
    const auto found = overrides.find(key);
    if (found != overrides.end()) target = std::stod(found->second);
  };
  const auto override_int = [&](const std::string& key, int& target) {
    const auto found = overrides.find(key);
    if (found != overrides.end()) target = std::atoi(found->second.c_str());
  };
  override_double("base_weight", settings.rules.base_weight);
  override_int("llm.start_limit", settings.rules.start_limit);
  override_int("llm.ceiling", settings.rules.ceiling);
  override_int("llm.on_battery_cap", settings.rules.on_battery_cap);
  override_double("llm.ram_floor_gb", settings.rules.ram_floor_gb);
  return settings;
}

std::string Profile::hash() const {
  return onion::log::blake2b_hex(onion::log::canonical_json(spec));
}

nlohmann::json Profile::to_json() const {
  return nlohmann::json{{"version", version},   {"prev", prev},
                        {"reason", reason},     {"changed", changed},
                        {"rules_version", rules_version},
                        {"schema", schema},     {"spec", spec}};
}

namespace {
// Records never contain floats (decision 0003): every measurement is an
// exact decimal string, identical on every OS.
std::string decimal(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.10f", value);
  std::string text(buffer);
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  return text.empty() ? "0" : text;
}
}  // namespace

std::optional<Profile> next_profile(
    const std::optional<Profile>& previous, const Scan& scan,
    const std::map<std::string, std::string>& overrides, std::string reason) {
  const DerivedSettings settings = derive(scan, overrides);
  const bool laptop = scan.on_battery.has_value();
  nlohmann::json spec = {
      {"scan",
       {{"os", scan.os},
        {"cores", scan.cores},
        {"ram_gb", decimal(scan.ram_gb)},
        {"disk_free_gb", decimal(scan.disk_free_gb)},
        {"durable_write_ms", decimal(scan.durable_write_ms)},
        {"gpu_fp64", scan.gpu_fp64},
        {"on_battery", scan.on_battery ? nlohmann::json(*scan.on_battery)
                                       : nlohmann::json(nullptr)},
        {"link_mbps", decimal(scan.link_mbps)},
        {"llm_rtt_ms", decimal(scan.llm_rtt_ms)}}},
      {"derived",
       {{"base_weight", decimal(settings.rules.base_weight)},
        {"start_limit", settings.rules.start_limit},
        {"ceiling", settings.rules.ceiling},
        {"cpu_pool", settings.rules.cpu_pool},
        {"on_battery_cap", settings.rules.on_battery_cap},
        {"ram_floor_gb", decimal(settings.rules.ram_floor_gb)},
        {"laptop", laptop}}},
      {"overrides", settings.overrides}};

  Profile next;
  next.version = previous ? previous->version + 1 : 1;
  next.prev = previous ? previous->hash() : "";
  next.reason = std::move(reason);
  next.rules_version = 1;
  next.schema = 1;
  next.spec = spec;

  if (previous && previous->hash() == next.hash()) return std::nullopt;
  if (previous) next.changed = diff(*previous, next);
  return next;
}

std::vector<std::string> diff(const Profile& older, const Profile& newer) {
  std::vector<std::string> changes;
  const auto& old_scan = older.spec["scan"];
  const auto& new_scan = newer.spec["scan"];
  for (const auto& key : {"cores", "ram_gb", "disk_free_gb",
                          "durable_write_ms", "gpu_fp64", "link_mbps",
                          "llm_rtt_ms", "os"}) {
    if (old_scan.value(key, nlohmann::json(nullptr)) !=
        new_scan.value(key, nlohmann::json(nullptr)))
      changes.push_back(std::string("scan.") + key);
  }
  const auto& old_derived = older.spec["derived"];
  const auto& new_derived = newer.spec["derived"];
  for (const auto& key : {"base_weight", "start_limit", "ceiling",
                          "cpu_pool", "on_battery_cap", "ram_floor_gb"}) {
    if (old_derived.value(key, nlohmann::json(nullptr)) !=
        new_derived.value(key, nlohmann::json(nullptr)))
      changes.push_back(std::string("derived.") + key);
  }
  if (older.spec["overrides"] != newer.spec["overrides"])
    changes.push_back("overrides");
  return changes;
}

}  // namespace onion::profile
