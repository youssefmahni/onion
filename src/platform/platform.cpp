#include "platform/platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06000000
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <iphlpapi.h>
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
}  // namespace

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
  return handle_ != INVALID_HANDLE_VALUE;
#else
  if (fd_ >= 0) ::close(fd_);
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR);
  return fd_ >= 0;
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
  return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE && FlushFileBuffers(static_cast<HANDLE>(handle_)) != 0;
#elif defined(__APPLE__)
  return fd_ >= 0 && ::fcntl(fd_, F_FULLFSYNC) == 0;
#else
  return fd_ >= 0 && ::fsync(fd_) == 0;
#endif
}
bool atomic_replace(const std::filesystem::path& tmp, const std::filesystem::path& dst) {
#ifdef _WIN32
  return MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return ::rename(tmp.c_str(), dst.c_str()) == 0;
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
  // Resolve GetIfTable2 at runtime: its table layout is SDK-version-sensitive.
  // A successful probe proves interface statistics are available; rate sampling
  // remains unknown until the profile module consumes the typed table in M5.
  using GetIfTable2Fn = DWORD (WINAPI*)(void**);
  using FreeMibTableFn = VOID (WINAPI*)(void*);
  if (const HMODULE iphlpapi = GetModuleHandleW(L"Iphlpapi.dll")) {
    const auto get_table = reinterpret_cast<GetIfTable2Fn>(GetProcAddress(iphlpapi, "GetIfTable2"));
    const auto free_table = reinterpret_cast<FreeMibTableFn>(GetProcAddress(iphlpapi, "FreeMibTable"));
    void* table = nullptr;
    if (get_table != nullptr && free_table != nullptr && get_table(&table) == NO_ERROR && table != nullptr) free_table(table);
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
