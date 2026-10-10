#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace onion::profile {
inline constexpr std::string_view component_name{"profile"};

// Hardware scan (DESIGN › Member profiles). Every value comes from the
// platform sampler or the GPU layer; nothing is invented.
struct Scan {
  std::string os;              // "linux" | "macos" | "windows"
  int cores{};
  double ram_gb{};
  double disk_free_gb{};       // data directory volume
  double durable_write_ms{};
  bool gpu_fp64{};
  std::optional<bool> on_battery;  // nullopt = desktop without battery
  double link_mbps{};
  double llm_rtt_ms{};         // round-trip to the provider /models
};

struct ScanOptions {
  std::string data_dir;        // for the durable-write probe
  std::string provider_url;    // base URL for the /models round-trip
  std::string api_key;         // never stored in the profile
  int timeout_s{10};
};

// Scan performs the I/O; tests inject the platform readings.
struct ScanInputs {
  std::string os;
  int cores{};
  double ram_gb{};
  double disk_free_gb{};
  double durable_write_ms{};
  bool gpu_fp64{};
  std::optional<bool> on_battery;
  double link_mbps{};
  double llm_rtt_ms{};
};

Scan make_scan(const ScanInputs& inputs);   // pure wrapper for fixtures

// The real scan: platform facts, the durable-write probe, GPU fp64 and the
// provider round-trip. Unknown values stay at their sentinel (-1 / nullopt).
Scan scan(const ScanOptions& options);

// Rules version 1 (DESIGN › Member profiles).
struct Rules {
  double base_weight{1.0};
  int start_limit{16};
  int ceiling{2000};
  int cpu_pool{};
  int on_battery_cap{-1};   // -1 = none
  double ram_floor_gb{2.0};
};

Rules rules_v1(bool laptop, int cores);

// node.conf: key = value lines, # comments. Overrides always win.
std::map<std::string, std::string> parse_node_conf(const std::string& text);

// The derived settings: rules version 1 plus the node.conf overrides.
struct DerivedSettings {
  Rules rules;
  std::map<std::string, std::string> overrides;
};

DerivedSettings derive(const Scan& scan,
                       const std::map<std::string, std::string>& overrides);

struct Profile {
  int version{};
  std::string prev;    // hash of the previous profile spec (empty = first)
  std::string reason;
  std::vector<std::string> changed;
  int rules_version{1};
  int schema{1};
  nlohmann::json spec;  // scan + derived settings

  nlohmann::json to_json() const;
  std::string hash() const;   // BLAKE2b-256 of the canonical spec
};

// A new MEMBER_PROFILE is emitted only when something changed (decision
// 0000's versioning rules); returns nullopt when nothing changed.
std::optional<Profile> next_profile(const std::optional<Profile>& previous,
                                    const Scan& scan,
                                    const std::map<std::string, std::string>& overrides,
                                    std::string reason);

// What changed between two profiles, as a human-readable list.
std::vector<std::string> diff(const Profile& older, const Profile& newer);

}  // namespace onion::profile
