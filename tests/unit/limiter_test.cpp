#include "llm/llm.hpp"

#include <gtest/gtest.h>

namespace {
onion::llm::LimitWindow healthy(double median_s = 1.0, std::uint64_t ended_at_s = 0) {
  onion::llm::LimitWindow window;
  window.ended_at_s = ended_at_s;
  window.calls = 100;
  window.median_latency_s = median_s;
  return window;
}

onion::llm::LimitWindow pressured(std::size_t rate_limited, std::size_t server_errors,
                                  std::size_t timeouts, std::uint64_t ended_at_s = 0) {
  onion::llm::LimitWindow window = healthy();
  window.ended_at_s = ended_at_s;
  window.rate_limited = rate_limited;
  window.server_errors = server_errors;
  window.timeouts = timeouts;
  return window;
}

TEST(Limiter, HalvesOnRateLimitAndHoldsOneWindow) {
  onion::llm::Limiter limiter(16, 2000);
  limiter.on_window(healthy());
  limiter.on_window(pressured(1, 0, 0));
  EXPECT_EQ(limiter.limit(), 10);  // First healthy window adds four, then pressure halves.
  limiter.on_window(healthy());
  EXPECT_EQ(limiter.limit(), 10);
  limiter.on_window(healthy());
  EXPECT_EQ(limiter.limit(), 14);
}

TEST(Limiter, HalvesOnServerError) {
  onion::llm::Limiter limiter(16, 2000);
  limiter.on_window(pressured(0, 1, 0));
  EXPECT_EQ(limiter.limit(), 8);
}

TEST(Limiter, HalvesOnlyWhenTimeoutsExceedFivePercent) {
  onion::llm::Limiter limiter(16, 2000);
  limiter.on_window(pressured(0, 0, 5));
  EXPECT_EQ(limiter.limit(), 20);
  limiter.on_window(pressured(0, 0, 6));
  EXPECT_EQ(limiter.limit(), 10);
}

TEST(Limiter, GrowsByFourAtHealthyLatency) {
  onion::llm::Limiter limiter(4, 2000);
  limiter.on_window(healthy(0.2, 0));
  EXPECT_EQ(limiter.low_load_baseline_s(), 0.2);
  EXPECT_EQ(limiter.limit(), 8);
  limiter.on_window(healthy(0.4, 3601));
  EXPECT_EQ(limiter.low_load_baseline_s(), 0.4);
  EXPECT_EQ(limiter.limit(), 12);
}

TEST(Limiter, HoldsWhenLatencyRisesWithoutErrors) {
  onion::llm::Limiter limiter(16, 2000);
  limiter.on_window(healthy(0.2, 0));
  limiter.on_window(healthy(0.31, 30));
  EXPECT_EQ(limiter.limit(), 20);
}

TEST(Limiter, NeverExceedsCeiling) {
  onion::llm::Limiter limiter(15, 16);
  limiter.on_window(healthy());
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(healthy());
  EXPECT_EQ(limiter.limit(), 16);

  onion::llm::Limiter minimum(1, 16);
  minimum.on_window(pressured(1, 0, 0));
  EXPECT_EQ(minimum.limit(), 1);
}
}  // namespace
