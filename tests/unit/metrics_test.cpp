#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "metrics/metrics.hpp"
#include "store/store.hpp"
#include "llm/llm.hpp"

namespace {

using onion::metrics::Store;

class MetricsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("onion_metrics_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
    Store::Options options;
    options.path = dir_ / "metrics.db";
    options.speed_retention = std::chrono::seconds(60);
    store_ = std::make_unique<Store>(options);
    std::string error;
    ASSERT_TRUE(store_->start(&error)) << error;
  }
  void TearDown() override {
    if (store_) store_->stop();
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }
  std::filesystem::path dir_;
  std::unique_ptr<Store> store_;
};

std::int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TEST_F(MetricsTest, SpeedSamplesRoundTripAndRetain) {
  std::string error;
  Store::SpeedSample sample;
  sample.ts = now_s();
  sample.inflight = 4;
  sample.limit = 2000;
  sample.answers_ps = 1.5;
  sample.out_tokens_ps = 1200.0;
  sample.p95_ms = 300.0;
  sample.errors = 1;
  ASSERT_TRUE(store_->record_speed(sample, &error)) << error;

  // An old sample (outside the 60 s retention) is pruned on the next
  // record, so only the fresh one is served.
  sample.ts = now_s() - 120;
  ASSERT_TRUE(store_->record_speed(sample, &error)) << error;

  const auto rows = store_->speed_since(now_s() - 30, &error);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].inflight, 4);
  EXPECT_EQ(rows[0].limit, 2000);
  EXPECT_DOUBLE_EQ(rows[0].answers_ps, 1.5);
  EXPECT_EQ(rows[0].errors, 1);
  EXPECT_EQ(store_->speed_retention_seconds(), 60);
}

TEST_F(MetricsTest, EventsRingAndCursor) {
  std::string error;
  ASSERT_TRUE(store_->append_event("CAMPAIGN", "{\"id\":\"c1\"}", &error));
  ASSERT_TRUE(store_->append_event("MEMBER_ADD", "{\"device\":\"d1\"}", &error));
  const auto all = store_->events_after(0, 10, &error);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].kind, "CAMPAIGN");
  EXPECT_EQ(all[1].kind, "MEMBER_ADD");

  const auto after_first = store_->events_after(all[0].seq, 10, &error);
  ASSERT_EQ(after_first.size(), 1u);
  EXPECT_EQ(after_first[0].kind, "MEMBER_ADD");
  EXPECT_EQ(store_->latest_event_seq(&error), all[1].seq);
}

TEST_F(MetricsTest, EventsRingBoundedToLatestTenThousand) {
  std::string error;
  for (int i = 0; i < 10005; ++i)
    ASSERT_TRUE(store_->append_event("TICK", std::to_string(i), &error));
  const auto rows = store_->events_after(0, 200, &error);
  ASSERT_EQ(rows.size(), 200u);
  EXPECT_EQ(rows.front().seq, 6);  // 10,000 kept, first page
}

TEST_F(MetricsTest, SpendBucketsSplitPeakOffpeak) {
  // A source db with two attempts, one peak-priced, one off-peak-priced.
  std::string error;
  onion::store::Db source(dir_ / "source.db");
  ASSERT_TRUE(source.open(&error)) << error;
  source.exec(
      "CREATE TABLE attempts(path TEXT, n INT, score TEXT, passed INT, "
      "tokens_in INT, tokens_cached INT, tokens_out INT, cost TEXT, "
      "campaign TEXT, hlc INT, origin TEXT);",
      &error);
  // 1000 in-tokens (500 miss / 500 hit), 100 out, cost 0.60.
  source.exec(
      "INSERT INTO attempts VALUES ('a', 1, '0.5', 1, 1000, 500, 100, "
      "'0.60', 'c1', 1, 'o');",
      &error);
  source.exec(
      "INSERT INTO attempts VALUES ('b', 1, '0.5', 1, 2000, 2000, 200, "
      "'0.40', 'c1', 2, 'o');",
      &error);

  onion::llm::PriceSchedule prices{
      {"0.30", "0.006", "1.20"}, {"0.15", "0.003", "0.60"}};
  ASSERT_TRUE(store_->refresh_spend(source, prices, &error)) << error;

  const auto hours = store_->spend_since(0, &error);
  ASSERT_EQ(hours.size(), 1u);
  EXPECT_EQ(hours[0].campaign, "c1");
  // 0.60 split by the peak/off-peak mix of attempt a; 0.40 by attempt b
  // (all cached => all off-peak): peak > 0 and offpeak > 0, summing to 1.00.
  const double peak = std::atof(hours[0].peak_usd.c_str());
  const double offpeak = std::atof(hours[0].offpeak_usd.c_str());
  EXPECT_NEAR(peak + offpeak, 1.00, 1e-6);
  EXPECT_GT(peak, 0.0);
  EXPECT_GT(offpeak, 0.0);
  source.close();
}
