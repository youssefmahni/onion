#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <limits>
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

// Static machine facts for the profile scan. Unknown values are nullopt.
std::string os_name();                       // "linux" | "macos" | "windows"
std::optional<double> total_ram_gb();
std::optional<double> disk_free_gb(const std::filesystem::path& path);
std::optional<double> link_speed_mbps();     // the default-route interface

// Raises the Unix soft descriptor limit to the hard limit where permitted, then
// returns the usable number of simultaneous outbound requests (64 descriptors
// remain reserved for logs, listeners, and other engine resources).
std::size_t outbound_request_ceiling();
std::size_t effective_open_file_limit();

class KeyStore {
 public:
  explicit KeyStore(std::filesystem::path directory);
  bool put(std::string_view name, std::string_view secret);
  std::optional<std::string> get(std::string_view name) const;
 private:
  std::filesystem::path directory_;
};

// A UDP datagram socket (SWIM gossip and the LAN beacon, DESIGN ›
// Transport: UDP 7946). Binds INADDR_ANY on the given port and optionally
// joins a multicast group on the default interface. The socket is
// non-blocking: recv() returns nullopt when nothing is pending.
class UdpSocket {
 public:
  UdpSocket();
  ~UdpSocket();
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;

  bool bind(std::uint16_t port, std::string* error);
  // Joins the group for the LAN beacon; binding must have succeeded.
  // Missing on routers that block multicast — callers fall back to seed
  // addresses (DESIGN › Discovery).
  bool join_group(const std::string& group, std::string* error);
  bool send(const std::string& address, std::uint16_t port,
            std::string_view bytes);

  // The locally bound port (the one assigned when bind() got port 0).
  std::optional<std::uint16_t> port() const;

  struct Datagram {
    std::string from_address;
    std::string bytes;
  };
  std::optional<Datagram> recv();

  // Closes the underlying socket (destructor, moves, and re-binds).
  void close();

 private:
#ifdef _WIN32
  // SOCKET is UINT_PTR; the invalid value is ~0 without including
  // winsock2.h in this header.
  std::uintptr_t socket_{std::numeric_limits<std::uintptr_t>::max()};
#else
  int socket_{-1};
#endif
};
}
