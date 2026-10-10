#pragma once

#include <string>

// Service definitions (M5): generated unit/plist/service files with golden
// content. Secrets come from the OS key store, never from Environment lines.
namespace onion::profile {

struct ServiceConfig {
  std::string binary;      // absolute path of data_depot
  std::string data_dir;    // absolute data directory
  std::string base_url;    // provider base URL
  std::string model;
  int api_port{8080};
};

// systemd unit (Linux): StateDirectory, Restart=always, no Environment secrets.
std::string systemd_unit(const ServiceConfig& config);
// launchd plist (macOS): KeepAlive.
std::string launchd_plist(const ServiceConfig& config);
// Windows: the service registration script (sc.exe create) — the service
// itself reads the key from the DPAPI-protected store.
std::string windows_service_script(const ServiceConfig& config);

}  // namespace onion::profile
