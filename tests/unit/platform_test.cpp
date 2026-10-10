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
TEST(DataDirTest, FlagOverrideWins) {
  const std::filesystem::path expected = "instance-data";
  EXPECT_EQ(onion::platform::data_dir(expected), expected);
}
TEST_F(PlatformTest, KeyStoreRoundTripsAndRejectsTraversal) {
  onion::platform::KeyStore store(root_ / "keys");
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
}  // namespace
