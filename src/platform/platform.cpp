#include "platform/platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <ifaddrs.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif
#endif

namespace onion::platform {
namespace {
SyncCounts sync_counts{};
std::optional<std::string> env(std::string_view name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, std::string(name).c_str()) != 0 || value == nullptr || *value == '\0') {
    std::free(value);
    return std::nullopt;
  }
  const std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(std::string(name).c_str());
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
#endif
}
bool valid_name(std::string_view name) {
  return !name.empty() && name.find("..") == std::string_view::npos &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_' || c == '-' || c == '.';
         });
}
#ifndef _WIN32
bool write_all(int fd, std::string_view bytes) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const auto written = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (written < 0) { if (errno == EINTR) continue; return false; }
    done += static_cast<std::size_t>(written);
  }
  return true;
}
#endif
bool private_dir(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) return false;
#ifndef _WIN32
  return ::chmod(path.c_str(), S_IRWXU) == 0;
#else
  return true;
#endif
}
#ifndef _WIN32
bool sync_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  if (ok) ++sync_counts.directory;
  return ok;
}
#endif
}  // namespace

void reset_sync_counts_for_test() { sync_counts = {}; }
SyncCounts sync_counts_for_test() { return sync_counts; }

std::filesystem::path data_dir(const std::optional<std::filesystem::path>& flag) {
  if (flag && !flag->empty()) return *flag;
  if (const auto value = env("ONION_DATA_DIR")) return *value;
#if defined(__linux__)
  if (const auto value = env("STATE_DIRECTORY")) return *value;
#endif
#ifdef _WIN32
  if (const auto value = env("LOCALAPPDATA")) return std::filesystem::path(*value) / "Onion";
#elif defined(__APPLE__)
  if (const auto value = env("HOME")) return std::filesystem::path(*value) / "Library" / "Application Support" / "Onion";
#else
  if (const auto value = env("XDG_DATA_HOME")) return std::filesystem::path(*value) / "onion";
  if (const auto value = env("HOME")) return std::filesystem::path(*value) / ".local" / "share" / "onion";
#endif
  return std::filesystem::path("onion-data");
}

AppendFile::~AppendFile() {
#ifdef _WIN32
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle_));
#else
  if (fd_ >= 0) ::close(fd_);
#endif
}
AppendFile::AppendFile(AppendFile&& other) noexcept {
#ifdef _WIN32
  handle_ = other.handle_; other.handle_ = nullptr;
#else
  fd_ = other.fd_; other.fd_ = -1;
#endif
}
AppendFile& AppendFile::operator=(AppendFile&& other) noexcept {
  if (this == &other) return *this;
#ifdef _WIN32
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = other.handle_; other.handle_ = nullptr;
#else
  if (fd_ >= 0) ::close(fd_);
  fd_ = other.fd_; other.fd_ = -1;
#endif
  return *this;
}
bool AppendFile::open(const std::filesystem::path& path) {
  std::error_code error; std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
#ifdef _WIN32
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle_ == INVALID_HANDLE_VALUE) return false;
  return true;
#else
  const bool existed = std::filesystem::exists(path);
  if (fd_ >= 0) ::close(fd_);
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR);
  return fd_ >= 0 && (existed || sync_directory(path.parent_path()));
#endif
}
bool AppendFile::append(std::string_view bytes) {
#ifdef _WIN32
  if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE) return false;
  std::size_t done = 0;
  while (done < bytes.size()) {
    const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), bytes.data() + done, count, &written, nullptr)) return false;
    done += written;
  }
  return true;
#else
  return fd_ >= 0 && write_all(fd_, bytes);
#endif
}
bool AppendFile::sync() {
#ifdef _WIN32
  const bool ok = handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE && FlushFileBuffers(static_cast<HANDLE>(handle_)) != 0;
  if (ok) ++sync_counts.file;
  return ok;
#elif defined(__APPLE__)
  if (fd_ < 0) return false;
  const bool ok = ::fcntl(fd_, F_FULLFSYNC) == 0 || ::fsync(fd_) == 0;
  if (ok) ++sync_counts.file;
  return ok;
#else
  const bool ok = fd_ >= 0 && ::fsync(fd_) == 0;
  if (ok) ++sync_counts.file;
  return ok;
#endif
}
bool atomic_replace(const std::filesystem::path& tmp, const std::filesystem::path& dst) {
#ifdef _WIN32
  return MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  const int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  if (!synced) return false;
  ++sync_counts.file;
  return ::rename(tmp.c_str(), dst.c_str()) == 0 && sync_directory(dst.parent_path());
#endif
}

Sample sample() {
  Sample result;
#ifdef _WIN32
  FILETIME idle{}, kernel{}, user{};
  if (GetSystemTimes(&idle, &kernel, &user)) {
    const auto ticks = [](const FILETIME& value) {
      return (static_cast<unsigned long long>(value.dwHighDateTime) << 32U) | value.dwLowDateTime;
    };
    struct Cpu { unsigned long long idle, total; };
    static std::optional<Cpu> previous;
    const Cpu current{ticks(idle), ticks(kernel) + ticks(user)};
    if (previous && current.total > previous->total)
      result.cpu_pct = 100.0 * (1.0 - static_cast<double>(current.idle - previous->idle) / static_cast<double>(current.total - previous->total));
    previous = current;
  }
  MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) result.ram_avail_gb = static_cast<double>(memory.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
  SYSTEM_POWER_STATUS power{};
  if (GetSystemPowerStatus(&power) && power.ACLineStatus != 255) result.on_battery = power.ACLineStatus == 0;
  PMIB_IF_TABLE2 table = nullptr;
  if (GetIfTable2(&table) == NO_ERROR && table != nullptr) {
    unsigned long long rx = 0;
    unsigned long long tx = 0;
    for (ULONG index = 0; index < table->NumEntries; ++index) {
      rx += table->Table[index].InOctets;
      tx += table->Table[index].OutOctets;
    }
    struct Net { unsigned long long rx, tx; std::chrono::steady_clock::time_point at; };
    static std::optional<Net> previous;
    const auto now = std::chrono::steady_clock::now();
    if (previous) {
      const double seconds = std::chrono::duration<double>(now - previous->at).count();
      if (seconds > 0) {
        result.net_rx_mbps = 8.0 * (rx - previous->rx) / seconds / 1'000'000.0;
        result.net_tx_mbps = 8.0 * (tx - previous->tx) / seconds / 1'000'000.0;
      }
    }
    previous = Net{rx, tx, now};
    FreeMibTable(table);
  }
#elif defined(__APPLE__)
  double loads[3]{};
  if (::getloadavg(loads, 3) > 0) result.load1 = loads[0];
  host_cpu_load_info_data_t cpu{}; mach_msg_type_number_t cpu_count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &cpu_count) == KERN_SUCCESS) {
    const auto total = static_cast<unsigned long long>(cpu.cpu_ticks[CPU_STATE_USER]) + cpu.cpu_ticks[CPU_STATE_SYSTEM] + cpu.cpu_ticks[CPU_STATE_IDLE] + cpu.cpu_ticks[CPU_STATE_NICE];
    struct Cpu { unsigned long long idle, total; }; static std::optional<Cpu> previous;
    const Cpu current{cpu.cpu_ticks[CPU_STATE_IDLE], total};
    if (previous && current.total > previous->total) result.cpu_pct = 100.0 * (1.0 - static_cast<double>(current.idle - previous->idle) / static_cast<double>(current.total - previous->total));
    previous = current;
  }
  std::uint64_t physical_memory = 0; std::size_t length = sizeof(physical_memory);
  (void)::sysctlbyname("hw.memsize", &physical_memory, &length, nullptr, 0);
  (void)physical_memory;
  vm_statistics64_data_t vm{}; mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  vm_size_t page_size = 0;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS && host_page_size(mach_host_self(), &page_size) == KERN_SUCCESS)
    result.ram_avail_gb = static_cast<double>(vm.free_count + vm.inactive_count) * page_size / (1024.0 * 1024.0 * 1024.0);
  struct ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) == 0) {
    unsigned long long rx = 0, tx = 0;
    for (auto* address = addresses; address != nullptr; address = address->ifa_next)
      if (address->ifa_addr != nullptr && address->ifa_addr->sa_family == AF_LINK && address->ifa_data != nullptr) { const auto* data = static_cast<if_data*>(address->ifa_data); rx += data->ifi_ibytes; tx += data->ifi_obytes; }
    struct Net { unsigned long long rx, tx; std::chrono::steady_clock::time_point at; }; static std::optional<Net> previous;
    const auto now = std::chrono::steady_clock::now();
    if (previous) { const double seconds = std::chrono::duration<double>(now - previous->at).count(); if (seconds > 0) { result.net_rx_mbps = 8.0 * (rx - previous->rx) / seconds / 1'000'000.0; result.net_tx_mbps = 8.0 * (tx - previous->tx) / seconds / 1'000'000.0; } }
    previous = Net{rx, tx, now}; freeifaddrs(addresses);
  }
#else
  std::ifstream load("/proc/loadavg"); double load1 = 0.0;
  if (load >> load1) result.load1 = load1;
  std::ifstream memory("/proc/meminfo"); std::string label, unit; double kib = 0.0;
  while (memory >> label >> kib >> unit) if (label == "MemAvailable:") { result.ram_avail_gb = kib / (1024.0 * 1024.0); break; }
  std::ifstream stat("/proc/stat"); std::string cpu; unsigned long long user = 0, nice = 0, system = 0, idle = 0, wait = 0, irq = 0, softirq = 0, steal = 0;
  if (stat >> cpu >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal && cpu == "cpu") {
    struct Cpu { unsigned long long idle, total; }; static std::optional<Cpu> previous;
    const Cpu current{idle + wait, user + nice + system + idle + wait + irq + softirq + steal};
    if (previous && current.total > previous->total) result.cpu_pct = 100.0 * (1.0 - static_cast<double>(current.idle - previous->idle) / static_cast<double>(current.total - previous->total));
    previous = current;
  }
  std::ifstream network("/proc/net/dev"); std::string line; unsigned long long rx = 0, tx = 0;
  while (std::getline(network, line)) { const auto colon = line.find(':'); if (colon == std::string::npos) continue; const auto name = line.substr(0, colon); if (name.find("lo") != std::string::npos) continue; std::istringstream values(line.substr(colon + 1)); unsigned long long received = 0, transmitted = 0, discard = 0; if (values >> received >> discard >> discard >> discard >> discard >> discard >> discard >> discard >> transmitted) { rx += received; tx += transmitted; } }
  struct Net { unsigned long long rx, tx; std::chrono::steady_clock::time_point at; }; static std::optional<Net> previous_net;
  const auto now = std::chrono::steady_clock::now(); if (previous_net && rx >= previous_net->rx && tx >= previous_net->tx) { const double seconds = std::chrono::duration<double>(now - previous_net->at).count(); if (seconds > 0) { result.net_rx_mbps = 8.0 * (rx - previous_net->rx) / seconds / 1'000'000.0; result.net_tx_mbps = 8.0 * (tx - previous_net->tx) / seconds / 1'000'000.0; } } previous_net = Net{rx, tx, now};
#endif
  return result;
}

KeyStore::KeyStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
bool KeyStore::put(std::string_view name, std::string_view secret) {
  if (!valid_name(name) || !private_dir(directory_)) return false;
  const auto path = directory_ / std::string(name);
#ifdef _WIN32
  DATA_BLOB input{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))}, output{};
  if (!CryptProtectData(&input, L"onion", nullptr, nullptr, nullptr, 0, &output)) return false;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (file) file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
  const bool ok = file.good(); LocalFree(output.pbData); return ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0) return false;
  const bool ok = write_all(fd, secret) && ::fsync(fd) == 0; ::close(fd);
  return ok && ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#endif
}
std::optional<std::string> KeyStore::get(std::string_view name) const {
  if (!valid_name(name)) return std::nullopt;
  std::ifstream file(directory_ / std::string(name), std::ios::binary);
  if (!file) return std::nullopt;
  const std::string encrypted((std::istreambuf_iterator<char>(file)), {});
#ifdef _WIN32
  if (encrypted.empty() || encrypted.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
  DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()))}, output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) return std::nullopt;
  std::string secret(reinterpret_cast<const char*>(output.pbData), output.cbData); LocalFree(output.pbData); return secret;
#else
  return encrypted;
#endif
}
}  // namespace onion::platform
