#include "platform/platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

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
#include <sys/resource.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#if defined(__APPLE__)
#include <limits.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/route.h>
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

bool sync_file_descriptor(int fd) {
#if defined(__APPLE__)
  return ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0;
#else
  return ::fsync(fd) == 0;
#endif
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
  const bool ok = sync_file_descriptor(fd_);
  if (ok) ++sync_counts.file;
  return ok;
#else
  const bool ok = fd_ >= 0 && sync_file_descriptor(fd_);
  if (ok) ++sync_counts.file;
  return ok;
#endif
}
bool atomic_replace(const std::filesystem::path& tmp, const std::filesystem::path& dst) {
#ifdef _WIN32
  const bool replaced = MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  // MOVEFILE_WRITE_THROUGH is the Windows durable-write equivalent for this
  // replacement. Count it as a file sync so the test hook covers this path.
  if (replaced) ++sync_counts.file;
  return replaced;
#else
  const int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool synced = sync_file_descriptor(fd);
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
  int mib[] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
  std::size_t bytes = 0;
  if (::sysctl(mib, 6, nullptr, &bytes, nullptr, 0) == 0 && bytes > 0) {
    std::vector<char> buffer(bytes);
    if (::sysctl(mib, 6, buffer.data(), &bytes, nullptr, 0) == 0) {
      unsigned long long rx = 0, tx = 0;
      for (char* cursor = buffer.data(); cursor < buffer.data() + bytes;) {
        const auto* header = reinterpret_cast<const if_msghdr2*>(cursor);
        if (header->ifm_msglen == 0) break;
        if (header->ifm_type == RTM_IFINFO2) { rx += header->ifm_data.ifi_ibytes; tx += header->ifm_data.ifi_obytes; }
        cursor += header->ifm_msglen;
      }
      struct Net { unsigned long long rx, tx; std::chrono::steady_clock::time_point at; }; static std::optional<Net> previous;
      const auto now = std::chrono::steady_clock::now();
      if (previous && rx >= previous->rx && tx >= previous->tx) { const double seconds = std::chrono::duration<double>(now - previous->at).count(); if (seconds > 0) { result.net_rx_mbps = 8.0 * (rx - previous->rx) / seconds / 1'000'000.0; result.net_tx_mbps = 8.0 * (tx - previous->tx) / seconds / 1'000'000.0; } }
      previous = Net{rx, tx, now};
    }
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
  while (std::getline(network, line)) { const auto colon = line.find(':'); if (colon == std::string::npos) continue; const auto name = line.substr(0, colon); const auto first = name.find_first_not_of(" \t"); const auto last = name.find_last_not_of(" \t"); if (first == std::string::npos || name.substr(first, last - first + 1) == "lo") continue; std::istringstream values(line.substr(colon + 1)); unsigned long long received = 0, transmitted = 0, discard = 0; if (values >> received >> discard >> discard >> discard >> discard >> discard >> discard >> discard >> transmitted) { rx += received; tx += transmitted; } }
  struct Net { unsigned long long rx, tx; std::chrono::steady_clock::time_point at; }; static std::optional<Net> previous_net;
  const auto now = std::chrono::steady_clock::now(); if (previous_net && rx >= previous_net->rx && tx >= previous_net->tx) { const double seconds = std::chrono::duration<double>(now - previous_net->at).count(); if (seconds > 0) { result.net_rx_mbps = 8.0 * (rx - previous_net->rx) / seconds / 1'000'000.0; result.net_tx_mbps = 8.0 * (tx - previous_net->tx) / seconds / 1'000'000.0; } } previous_net = Net{rx, tx, now};
#endif
  return result;
}

std::size_t outbound_request_ceiling() {
#ifdef _WIN32
  // Windows does not expose an RLIMIT_NOFILE equivalent for Winsock sockets.
  // The caller's configured ceiling is therefore left unchanged.
  return std::numeric_limits<std::size_t>::max();
#else
  rlimit limits{};
  if (::getrlimit(RLIMIT_NOFILE, &limits) != 0) return 1;
#if defined(__APPLE__)
  std::uint64_t per_process = 0;
  std::size_t bytes = sizeof(per_process);
  if (::sysctlbyname("kern.maxfilesperproc", &per_process, &bytes, nullptr, 0) != 0) per_process = limits.rlim_max;
  const rlim_t cap = static_cast<rlim_t>(std::min<std::uint64_t>(
      {static_cast<std::uint64_t>(limits.rlim_max), per_process, static_cast<std::uint64_t>(OPEN_MAX)}));
  if (limits.rlim_cur < cap) {
    rlimit raised = limits;
    raised.rlim_cur = cap;
    (void)::setrlimit(RLIMIT_NOFILE, &raised);
    (void)::getrlimit(RLIMIT_NOFILE, &limits);
  }
#else
  if (limits.rlim_cur < limits.rlim_max) {
    rlimit raised = limits;
    raised.rlim_cur = raised.rlim_max;
    (void)::setrlimit(RLIMIT_NOFILE, &raised);
    (void)::getrlimit(RLIMIT_NOFILE, &limits);
  }
#endif
  if (limits.rlim_cur <= 64) return 1;
  const auto usable = limits.rlim_cur - 64;
  return usable > static_cast<rlim_t>(std::numeric_limits<std::size_t>::max())
      ? std::numeric_limits<std::size_t>::max()
      : static_cast<std::size_t>(usable);
#endif
}

std::size_t effective_open_file_limit() {
#ifdef _WIN32
  return std::numeric_limits<std::size_t>::max();
#else
  rlimit limits{};
  return ::getrlimit(RLIMIT_NOFILE, &limits) == 0
      ? static_cast<std::size_t>(limits.rlim_cur) : 0;
#endif
}

KeyStore::KeyStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

#if defined(__APPLE__)
// macOS: the Keychain, through the OS `security` tool (fork/exec, no shell).
namespace {
std::optional<std::string> security_tool(const std::vector<std::string>& args,
                                         bool capture_output) {
  int pipefd[2] = {-1, -1};
  if (capture_output && ::pipe(pipefd) != 0) return std::nullopt;
  const pid_t child = ::fork();
  if (child == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("security"));
    for (const std::string& arg : args)
      argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    if (capture_output) {
      ::dup2(pipefd[1], 1);
      ::close(pipefd[0]);
    }
    ::execvp("security", argv.data());
    ::_exit(127);
  }
  if (child < 0) {
    if (capture_output) { ::close(pipefd[0]); ::close(pipefd[1]); }
    return std::nullopt;
  }
  std::string output;
  if (capture_output) {
    ::close(pipefd[1]);
    char buffer[256];
    ssize_t read_bytes;
    while ((read_bytes = ::read(pipefd[0], buffer, sizeof(buffer))) > 0)
      output.append(buffer, static_cast<std::size_t>(read_bytes));
    ::close(pipefd[0]);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
  if (capture_output) {
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
      output.pop_back();
    return output;
  }
  return std::string{};
}
}  // namespace
#endif

bool KeyStore::put(std::string_view name, std::string_view secret) {
  if (!valid_name(name) || !private_dir(directory_)) return false;
  const auto path = directory_ / std::string(name);
#ifdef _WIN32
  DATA_BLOB input{static_cast<DWORD>(secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))}, output{};
  if (!CryptProtectData(&input, L"onion", nullptr, nullptr, nullptr, 0, &output)) return false;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (file) file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
  const bool ok = file.good(); LocalFree(output.pbData); return ok;
#elif defined(__APPLE__)
  // The Keychain is the primary store; the 0600 file is always written as
  // the fallback. The keychain persists across processes (and across CI
  // jobs on hosted runners), so any stale item must be deleted first —
  // otherwise a lookup can return a PREVIOUS device's secret, silently
  // cloning another identity.
  security_tool({"delete-generic-password", "-s", "onion", "-a",
                 std::string(name)},
                false);
  security_tool({"add-generic-password", "-U", "-s", "onion", "-a",
                 std::string(name), "-w", std::string(secret)},
                false);
  {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    const bool ok = write_all(fd, secret) && ::fsync(fd) == 0;
    ::close(fd);
    return ok && ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
  }
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0) return false;
  const bool ok = write_all(fd, secret) && ::fsync(fd) == 0; ::close(fd);
  return ok && ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#endif
}
std::optional<std::string> KeyStore::get(std::string_view name) const {
  if (!valid_name(name)) return std::nullopt;
#if defined(__APPLE__)
  {
    // The 0600 file is the source of truth (deterministic, permission
    // checked): the shared login keychain on CI runners survives across
    // jobs and machines, and parallel nodes on one machine collide on the
    // same service/account item — reading it first silently returned
    // another device's key (the macOS BadSig root cause). The keychain is
    // only a migration fallback for installs that predate the file.
    struct stat status{};
    const auto path = directory_ / std::string(name);
    if (::stat(path.c_str(), &status) == 0 && (status.st_mode & 0077) == 0) {
      std::ifstream file(path, std::ios::binary);
      if (file)
        return std::string((std::istreambuf_iterator<char>(file)), {});
    }
  }
  return security_tool({"find-generic-password", "-s", "onion", "-a",
                        std::string(name), "-w"},
                       true);
#elif defined(_WIN32)
  std::ifstream file(directory_ / std::string(name), std::ios::binary);
  if (!file) return std::nullopt;
  const std::string encrypted((std::istreambuf_iterator<char>(file)), {});
  if (encrypted.empty() || encrypted.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
  DATA_BLOB input{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()))}, output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) return std::nullopt;
  std::string secret(reinterpret_cast<const char*>(output.pbData), output.cbData); LocalFree(output.pbData); return secret;
#else
  struct stat status{};
  const auto path = directory_ / std::string(name);
  if (::stat(path.c_str(), &status) != 0 || (status.st_mode & 0077) != 0) return std::nullopt;
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(file)), {});
#endif
}

// ------------------------------------------------- machine facts (M5)

std::string os_name() {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

std::optional<double> total_ram_gb() {
#ifdef _WIN32
  MEMORYSTATUSEX memory;
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory))
    return static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
  return std::nullopt;
#elif defined(__APPLE__)
  std::int64_t bytes = 0;
  std::size_t size = sizeof(bytes);
  if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0)
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  return std::nullopt;
#else
  std::ifstream in("/proc/meminfo");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("MemTotal:", 0) == 0) {
      long long kilobytes = 0;
      std::sscanf(line.c_str() + 9, "%lld", &kilobytes);
      return static_cast<double>(kilobytes) / (1024.0 * 1024.0);
    }
  }
  return std::nullopt;
#endif
}

std::optional<double> disk_free_gb(const std::filesystem::path& path) {
#ifdef _WIN32
  ULARGE_INTEGER free_bytes;
  if (GetDiskFreeSpaceExW(path.c_str(), &free_bytes, nullptr, nullptr))
    return static_cast<double>(free_bytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
  return std::nullopt;
#else
  struct statvfs stats;
  if (::statvfs(path.c_str(), &stats) == 0)
    return static_cast<double>(stats.f_bavail) * stats.f_frsize /
           (1024.0 * 1024.0 * 1024.0);
  return std::nullopt;
#endif
}

std::optional<double> link_speed_mbps() {
#ifdef _WIN32
  return std::nullopt;  // Windows fleet profiles arrive with the M9 joins
#elif defined(__APPLE__)
  // The default route interface speed via sysctl net.link... best effort.
  return std::nullopt;
#else
  // The default-route interface from /proc/net/route, speed from sysfs.
  std::string interface;
  {
    std::ifstream in("/proc/net/route");
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      std::string iface, destination;
      fields >> iface >> destination;
      if (destination == "00000000") {
        interface = iface;
        break;
      }
    }
  }
  if (interface.empty()) return std::nullopt;
  std::ifstream speed("/sys/class/net/" + interface + "/speed");
  double mbps = 0;
  if (speed >> mbps) return mbps;
  return std::nullopt;
#endif
}

}  // namespace onion::platform

// ---------------------------------------------------------------------------
// UdpSocket (SWIM gossip + LAN beacon)
// ---------------------------------------------------------------------------

namespace onion::platform {

namespace {

#ifdef _WIN32
void ensure_winsock() {
  static bool initialized = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)initialized;
}

std::string last_socket_error() {
  const int code = WSAGetLastError();
  return "winsock error " + std::to_string(code);
}
#else
std::string last_socket_error() { return std::strerror(errno); }

// Fill the sockaddr for an IPv4 host string; returns false on garbage.
bool ipv4_sockaddr(const std::string& address, std::uint16_t port,
                   sockaddr_in* out, std::string* error) {
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(port);
  if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) == 1) {
    *out = target;
    return true;
  }
  // Not a literal address: resolve as a host name.
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(address.c_str(), nullptr, &hints, &result) != 0 ||
      result == nullptr) {
    if (error) *error = "unresolvable address: " + address;
    return false;
  }
  target.sin_addr =
      reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
  freeaddrinfo(result);
  *out = target;
  return true;
}
#endif

}  // namespace

UdpSocket::UdpSocket() = default;

void UdpSocket::close() {
#ifdef _WIN32
  if (socket_ != INVALID_SOCKET) {
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }
#else
  if (socket_ >= 0) {
    ::close(socket_);
    socket_ = -1;
  }
#endif
}

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : socket_(other.socket_) {
#ifdef _WIN32
  other.socket_ = INVALID_SOCKET;
#else
  other.socket_ = -1;
#endif
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    close();
    socket_ = other.socket_;
#ifdef _WIN32
    other.socket_ = INVALID_SOCKET;
#else
    other.socket_ = -1;
#endif
  }
  return *this;
}

bool UdpSocket::bind(std::uint16_t port, std::string* error) {
  close();  // a re-bind replaces any previous socket
#ifdef _WIN32
  ensure_winsock();
  socket_ = static_cast<std::uintptr_t>(
      ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (socket_ == INVALID_SOCKET) {
    if (error) *error = last_socket_error();
    return false;
  }
  u_long nonblocking = 1;
  if (::ioctlsocket(socket_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
    if (error) *error = last_socket_error();
    close();
    return false;
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (::bind(socket_, reinterpret_cast<sockaddr*>(&local),
             sizeof(local)) == SOCKET_ERROR) {
    if (error) *error = last_socket_error();
    return false;
  }
  return true;
#else
  socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ < 0) {
    if (error) *error = last_socket_error();
    return false;
  }
  // Non-blocking so the node loop can poll between ticks.
  const int flags = ::fcntl(socket_, F_GETFL, 0);
  ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (::bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) !=
      0) {
    if (error) *error = last_socket_error();
    return false;
  }
  return true;
#endif
}

bool UdpSocket::join_group(const std::string& group, std::string* error) {
#ifdef _WIN32
  ip_mreq membership{};
  membership.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
  membership.imr_interface.s_addr = htonl(INADDR_ANY);
  if (::setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&membership),
                   sizeof(membership)) == SOCKET_ERROR) {
    if (error) *error = last_socket_error();
    return false;
  }
  return true;
#else
#if defined(__linux__)
  ip_mreqn membership{};
  membership.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
  membership.imr_address.s_addr = htonl(INADDR_ANY);
  membership.imr_ifindex = 0;
#else
  // macOS and other BSDs only define ip_mreq (whose interface field is
  // imr_interface, not ip_mreqn's imr_address).
  ip_mreq membership{};
  membership.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
  membership.imr_interface.s_addr = htonl(INADDR_ANY);
#endif
  if (::setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                   sizeof(membership)) != 0) {
    if (error) *error = last_socket_error();
    return false;
  }
  return true;
#endif
}

bool UdpSocket::send(const std::string& address, std::uint16_t port,
                     std::string_view bytes) {
#ifdef _WIN32
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(port);
  target.sin_addr.s_addr = ::inet_addr(address.c_str());
  if (target.sin_addr.s_addr == INADDR_NONE) return false;
  return ::sendto(socket_, bytes.data(), static_cast<int>(bytes.size()), 0,
                  reinterpret_cast<sockaddr*>(&target), sizeof(target)) !=
         SOCKET_ERROR;
#else
  sockaddr_in target{};
  std::string resolve_error;
  if (!ipv4_sockaddr(address, port, &target, &resolve_error)) return false;
  // Datagrams are atomic: sendto either queues the whole datagram or
  // fails; a partial send never happens for SOCK_DGRAM.
  const ssize_t sent =
      ::sendto(socket_, bytes.data(), bytes.size(), 0,
               reinterpret_cast<sockaddr*>(&target), sizeof(target));
  return sent == static_cast<ssize_t>(bytes.size());
#endif
}

std::optional<std::uint16_t> UdpSocket::port() const {
#ifdef _WIN32
  sockaddr_in local{};
  int length = sizeof(local);
  if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length) ==
      SOCKET_ERROR)
    return std::nullopt;
#else
  sockaddr_in local{};
  socklen_t length = sizeof(local);
  if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length) !=
      0)
    return std::nullopt;
#endif
  return ntohs(local.sin_port);
}

std::optional<UdpSocket::Datagram> UdpSocket::recv() {
  char buffer[65536];
#ifdef _WIN32
  sockaddr_in from{};
  int from_length = sizeof(from);
  const int received =
      ::recvfrom(socket_, buffer, sizeof(buffer), 0,
                 reinterpret_cast<sockaddr*>(&from), &from_length);
  if (received == SOCKET_ERROR) return std::nullopt;
  Datagram datagram;
  datagram.from_address = ::inet_ntoa(from.sin_addr);
  datagram.bytes.assign(buffer, static_cast<std::size_t>(received));
  return datagram;
#else
  sockaddr_in from{};
  socklen_t from_length = sizeof(from);
  const ssize_t received =
      ::recvfrom(socket_, buffer, sizeof(buffer), 0,
                 reinterpret_cast<sockaddr*>(&from), &from_length);
  if (received < 0) return std::nullopt;  // EAGAIN/EWOULDBLOCK or an error
  Datagram datagram;
  char host[INET_ADDRSTRLEN]{};
  ::inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
  datagram.from_address = host;
  datagram.bytes.assign(buffer, static_cast<std::size_t>(received));
  return datagram;
#endif
}

}  // namespace onion::platform (udp)
