#pragma once

#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace onion::platform {
inline constexpr std::string_view component_name{"platform"};

std::filesystem::path data_dir(
    const std::optional<std::filesystem::path>& flag_override = std::nullopt);

class AppendFile {
 public:
  AppendFile() = default;
  ~AppendFile();
  AppendFile(const AppendFile&) = delete;
  AppendFile& operator=(const AppendFile&) = delete;
  AppendFile(AppendFile&& other) noexcept;
  AppendFile& operator=(AppendFile&& other) noexcept;
  bool open(const std::filesystem::path& path);
  bool append(std::string_view bytes);
  bool sync();
 private:
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

bool atomic_replace(const std::filesystem::path& tmp,
                    const std::filesystem::path& destination);

struct SyncCounts { unsigned file = 0; unsigned directory = 0; };
void reset_sync_counts_for_test();
SyncCounts sync_counts_for_test();

struct Sample {
  std::optional<double> load1, cpu_pct, ram_avail_gb, net_rx_mbps, net_tx_mbps, gpu_pct;
  std::optional<bool> on_battery;
};
Sample sample();

// Raises the Unix soft descriptor limit to the hard limit where permitted, then
// returns the usable number of simultaneous outbound requests (64 descriptors
// remain reserved for logs, listeners, and other engine resources).
std::size_t outbound_request_ceiling();

class KeyStore {
 public:
  explicit KeyStore(std::filesystem::path directory);
  bool put(std::string_view name, std::string_view secret);
  std::optional<std::string> get(std::string_view name) const;
 private:
  std::filesystem::path directory_;
};
}
