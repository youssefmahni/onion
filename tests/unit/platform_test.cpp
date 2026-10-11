#include "platform/platform.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#include <cstdlib>
#else
#include <cstdlib>
#endif

namespace {
class PlatformTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / ("onion-platform-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root_));
  }
  void TearDown() override { std::error_code error; std::filesystem::remove_all(root_, error); }
  std::filesystem::path root_;
};
TEST_F(PlatformTest, DurableAppendSurvivesReopen) {
  const auto log = root_ / "logs" / "events.log";
  { onion::platform::AppendFile file; ASSERT_TRUE(file.open(log)); ASSERT_TRUE(file.append("one\\n")); ASSERT_TRUE(file.sync()); }
  { onion::platform::AppendFile file; ASSERT_TRUE(file.open(log)); ASSERT_TRUE(file.append("two\\n")); ASSERT_TRUE(file.sync()); }
  std::ifstream input(log, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "one\\ntwo\\n");
}
TEST_F(PlatformTest, AtomicReplaceReplacesDestination) {
  const auto temporary = root_ / "new.tmp"; const auto target = root_ / "target";
  { std::ofstream output(temporary, std::ios::binary); output << "new"; }
  { std::ofstream output(target, std::ios::binary); output << "old"; }
  ASSERT_TRUE(onion::platform::atomic_replace(temporary, target));
  EXPECT_FALSE(std::filesystem::exists(temporary));
  std::ifstream input(target, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "new");
}
TEST_F(PlatformTest, DurableOperationsSyncFileAndDirectory) {
  onion::platform::reset_sync_counts_for_test();
  const auto source = root_ / "source";
  { onion::platform::AppendFile file; ASSERT_TRUE(file.open(source)); ASSERT_TRUE(file.append("x")); ASSERT_TRUE(file.sync()); }
  const auto destination = root_ / "destination";
  ASSERT_TRUE(onion::platform::atomic_replace(source, destination));
  const auto counts = onion::platform::sync_counts_for_test();
  EXPECT_GE(counts.file, 2U);
#ifndef _WIN32
  EXPECT_GE(counts.directory, 2U);
#endif
}
TEST(DataDirTest, FlagOverrideWins) {
  const std::filesystem::path expected = "instance-data";
  EXPECT_EQ(onion::platform::data_dir(expected), expected);
}
TEST(DataDirTest, EnvironmentPrecedence) {
#ifdef _WIN32
  _putenv_s("ONION_DATA_DIR", "onion-env");
  _putenv_s("LOCALAPPDATA", "local-app-data");
#else
  setenv("ONION_DATA_DIR", "onion-env", 1);
  setenv("HOME", "home-dir", 1);
  setenv("XDG_DATA_HOME", "xdg-data", 1);
#if defined(__linux__)
  setenv("STATE_DIRECTORY", "state-dir", 1);
#endif
#endif
  EXPECT_EQ(onion::platform::data_dir(), std::filesystem::path("onion-env"));
#ifdef _WIN32
  _putenv_s("ONION_DATA_DIR", "");
  EXPECT_EQ(onion::platform::data_dir(), std::filesystem::path("local-app-data") / "Onion");
#elif defined(__linux__)
  unsetenv("ONION_DATA_DIR");
  EXPECT_EQ(onion::platform::data_dir(), std::filesystem::path("state-dir"));
  unsetenv("STATE_DIRECTORY");
  EXPECT_EQ(onion::platform::data_dir(), std::filesystem::path("xdg-data") / "onion");
  unsetenv("XDG_DATA_HOME");
  EXPECT_EQ(onion::platform::data_dir(), std::filesystem::path("home-dir") / ".local" / "share" / "onion");
#else
  unsetenv("ONION_DATA_DIR");
  EXPECT_EQ(onion::platform::data_dir(), std::filesystem::path("home-dir") / "Library" / "Application Support" / "Onion");
#endif
}
TEST_F(PlatformTest, KeyStoreRoundTripsAndRejectsTraversal) {
  onion::platform::KeyStore store(root_ / "keys");
  // The 0600 file is the source of truth on every OS (macOS included:
  // the keychain is only a migration fallback, so no unlocked login
  // keychain is needed — this test runs everywhere, no skip).
  ASSERT_TRUE(store.put("device.key", "secret"));
  EXPECT_EQ(store.get("device.key"), std::optional<std::string>("secret"));
  EXPECT_FALSE(store.put("../outside", "bad"));
#ifndef _WIN32
  struct stat directory{}; struct stat key{};
  ASSERT_EQ(::stat((root_ / "keys").c_str(), &directory), 0);
  ASSERT_EQ(::stat((root_ / "keys" / "device.key").c_str(), &key), 0);
  EXPECT_EQ(directory.st_mode & 0777, 0700);
  EXPECT_EQ(key.st_mode & 0777, 0600);
  ASSERT_EQ(::chmod((root_ / "keys" / "device.key").c_str(), 0644), 0);
  EXPECT_FALSE(store.get("device.key"));
#endif
}
TEST(PlatformSampleTest, OSValuesArePresent) {
  (void)onion::platform::sample();
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const auto value = onion::platform::sample();
#ifdef _WIN32
  EXPECT_FALSE(value.load1); ASSERT_TRUE(value.cpu_pct); ASSERT_TRUE(value.ram_avail_gb); ASSERT_TRUE(value.net_rx_mbps);
#else
  ASSERT_TRUE(value.load1); ASSERT_TRUE(value.cpu_pct); ASSERT_TRUE(value.ram_avail_gb); ASSERT_TRUE(value.net_rx_mbps);
#endif
  EXPECT_GE(*value.net_rx_mbps, 0.0);
}
TEST(PlatformFileLimits, ReservesDescriptorsForEngineResources) {
  EXPECT_GE(onion::platform::outbound_request_ceiling(), 1U);
}
#if defined(__APPLE__)
TEST(PlatformFileLimits, RaisesMacOSSoftLimitToAtLeast4096) {
  (void)onion::platform::outbound_request_ceiling();
  EXPECT_GE(onion::platform::effective_open_file_limit(), 4096U);
}
#endif
}  // namespace

TEST(PlatformUdpTest, LoopbackSendAndRecv) {
  onion::platform::UdpSocket a;
  onion::platform::UdpSocket b;
  std::string error;
  ASSERT_TRUE(a.bind(0, &error)) << error;
  ASSERT_TRUE(b.bind(0, &error)) << error;
  const auto a_port = a.port();
  const auto b_port = b.port();
  ASSERT_TRUE(a_port.has_value());
  ASSERT_TRUE(b_port.has_value());
  ASSERT_NE(*a_port, *b_port);

  const std::string ping = R"({"t":"ping","from":"dev-a","seq":1})";
  ASSERT_TRUE(a.send("127.0.0.1", *b_port, ping));
  // Loopback delivery can lag a loaded runner: wait bounded, then read.
  std::optional<onion::platform::UdpSocket::Datagram> datagram;
  for (int attempt = 0; attempt < 200 && !datagram; attempt++) {
    datagram = b.recv();
    if (!datagram)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(datagram.has_value());
  EXPECT_EQ(datagram->bytes, ping);
  EXPECT_EQ(datagram->from_address, "127.0.0.1");

  // And back the other way.
  const std::string ack = R"({"t":"ack","from":"dev-b","seq":1})";
  ASSERT_TRUE(b.send("127.0.0.1", *a_port, ack));
  std::optional<onion::platform::UdpSocket::Datagram> reply;
  for (int attempt = 0; attempt < 200 && !reply; attempt++) {
    reply = a.recv();
    if (!reply)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->bytes, ack);
  // An empty socket polls clean.
  EXPECT_FALSE(b.recv().has_value());
}

TEST(PlatformUdpTest, MulticastJoinFailsGracefullyWithoutTheGroup) {
  onion::platform::UdpSocket socket;
  std::string error;
  ASSERT_TRUE(socket.bind(0, &error)) << error;
  // Whether the host can join the group depends on the environment
  // (containers often lack multicast); either way the call must not
  // crash and must report a reason on failure.
  if (!socket.join_group("239.255.77.31", &error))
    EXPECT_FALSE(error.empty());
}
