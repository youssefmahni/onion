#include <gtest/gtest.h>

#include <map>
#include <string>

#include "profile/profile.hpp"

namespace {

using onion::profile::Profile;
using onion::profile::Rules;
using onion::profile::Scan;
using onion::profile::ScanInputs;
using onion::profile::derive;
using onion::profile::diff;
using onion::profile::make_scan;
using onion::profile::next_profile;
using onion::profile::parse_node_conf;
using onion::profile::rules_v1;

ScanInputs desktop_inputs() {
  return {"linux", 12, 64.0, 500.0, 1.2, true, std::nullopt, 1000.0, 30.0};
}

ScanInputs laptop_inputs() {
  return {"macos", 8, 16.0, 200.0, 2.0, false, true, 400.0, 40.0};
}

TEST(ProfileRules, DesktopAndLaptopDefaults) {
  const Rules desktop = rules_v1(false, 12);
  EXPECT_DOUBLE_EQ(desktop.base_weight, 1.0);
  EXPECT_EQ(desktop.start_limit, 16);
  EXPECT_EQ(desktop.ceiling, 2000);
  EXPECT_EQ(desktop.on_battery_cap, -1);
  EXPECT_DOUBLE_EQ(desktop.ram_floor_gb, 2.0);

  const Rules laptop = rules_v1(true, 8);
  EXPECT_DOUBLE_EQ(laptop.base_weight, 0.25);
  EXPECT_EQ(laptop.start_limit, 4);
  EXPECT_EQ(laptop.ceiling, 32);
  EXPECT_EQ(laptop.on_battery_cap, 8);
  EXPECT_DOUBLE_EQ(laptop.ram_floor_gb, 1.0);
}

TEST(ProfileNodeConf, ParsesOverridesAndComments) {
  const auto values = parse_node_conf(
      "# comment\nbase_weight = 2.5\nllm.ceiling=64\n\nllm.timeout_s = 30\n");
  EXPECT_EQ(values.at("base_weight"), "2.5");
  EXPECT_EQ(values.at("llm.ceiling"), "64");
  EXPECT_EQ(values.at("llm.timeout_s"), "30");
  EXPECT_EQ(values.size(), 3u);
}

TEST(ProfileDerive, OverridesAlwaysWin) {
  const Scan scan = make_scan(desktop_inputs());
  const auto plain = derive(scan, {});
  EXPECT_DOUBLE_EQ(plain.rules.base_weight, 1.0);
  const auto overridden =
      derive(scan, {{"base_weight", "3.0"}, {"llm.ceiling", "99"}});
  EXPECT_DOUBLE_EQ(overridden.rules.base_weight, 3.0);
  EXPECT_EQ(overridden.rules.ceiling, 99);
}

TEST(ProfileVersions, NewProfileOnlyOnChange) {
  const Scan desktop = make_scan(desktop_inputs());
  const auto first = next_profile(std::nullopt, desktop, {}, "start");
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->version, 1);
  EXPECT_TRUE(first->prev.empty());

  // Same scan, same overrides: no new version.
  const auto same = next_profile(first, desktop, {}, "refresh");
  EXPECT_FALSE(same.has_value());

  // A changed scan: version 2, chained to the first.
  const auto changed = next_profile(first, make_scan(laptop_inputs()), {},
                                    "battery detected");
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(changed->version, 2);
  EXPECT_EQ(changed->prev, first->hash());
  EXPECT_NE(changed->hash(), first->hash());
  const auto changes = diff(*first, *changed);
  EXPECT_NE(std::find(changes.begin(), changes.end(), "scan.os"),
            changes.end());
}

TEST(ProfileVersions, OverrideChangeBumpsVersion) {
  const Scan desktop = make_scan(desktop_inputs());
  const auto first = next_profile(std::nullopt, desktop, {}, "start");
  const auto overridden =
      next_profile(first, desktop, {{"llm.ceiling", "42"}}, "user pin");
  ASSERT_TRUE(overridden.has_value());
  EXPECT_EQ(overridden->version, 2);
  EXPECT_EQ(overridden->prev, first->hash());
}

}  // namespace

// ----------------------------------------------------------------- services

#include "profile/service.hpp"

TEST(ServiceFiles, GoldenContent) {
  onion::profile::ServiceConfig config;
  config.binary = "/usr/local/bin/data_depot";
  config.data_dir = "/var/lib/onion";
  config.base_url = "https://api.deepseek.com";
  config.model = "deepseek-flash";
  config.api_port = 8080;

  const std::string unit = onion::profile::systemd_unit(config);
  EXPECT_NE(unit.find("Restart=always"), std::string::npos);
  EXPECT_NE(unit.find("StateDirectory=onion"), std::string::npos);
  EXPECT_NE(unit.find("--data-dir /var/lib/onion"), std::string::npos);
  // No inline secrets: the key comes from the OS key store.
  EXPECT_EQ(unit.find("Environment="), std::string::npos);
  EXPECT_EQ(unit.find("DEEPSEEK"), std::string::npos);
  EXPECT_EQ(unit.find("api_key"), std::string::npos);

  const std::string plist = onion::profile::launchd_plist(config);
  EXPECT_NE(plist.find("<key>KeepAlive</key><true/>"), std::string::npos);
  EXPECT_NE(plist.find("com.onion.node"), std::string::npos);

  const std::string script = onion::profile::windows_service_script(config);
  EXPECT_NE(script.find("sc create OnionNode"), std::string::npos);
  EXPECT_NE(script.find("start= auto"), std::string::npos);
}

TEST(ServiceFiles, EscapeXmlInPlist) {
  onion::profile::ServiceConfig config;
  config.binary = "/usr/bin/a&b";
  config.data_dir = "/tmp/<data>";
  const std::string plist = onion::profile::launchd_plist(config);
  EXPECT_NE(plist.find("a&amp;b"), std::string::npos);
  EXPECT_NE(plist.find("&lt;data&gt;"), std::string::npos);
  EXPECT_EQ(plist.find("<data>"), std::string::npos);
}
