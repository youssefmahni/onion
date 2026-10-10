#include "llm/llm.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {
TEST(Budget, ConcurrentReservationsNeverOverspend) {
  onion::llm::Budget budget("1.00");
  std::atomic<int> accepted{};
  std::vector<std::thread> threads;
  for (int index = 0; index < 500; ++index) {
    threads.emplace_back([&] { if (budget.reserve("0.01")) ++accepted; });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(accepted, 100);
  EXPECT_EQ(budget.reserved_usd(), "1");
  EXPECT_EQ(budget.remaining_usd(), "0");
}

TEST(Budget, SettlementReleasesUnusedReservation) {
  onion::llm::Budget budget("1.00");
  auto reservation = budget.reserve("0.80");
  ASSERT_TRUE(reservation);
  budget.settle(*reservation, "0.25");
  EXPECT_EQ(budget.spent_usd(), "0.25");
  EXPECT_EQ(budget.reserved_usd(), "0");
  EXPECT_EQ(budget.remaining_usd(), "0.75");
}

TEST(Metering, UsesOffpeakOnlyForCallsInsideOneWindow) {
  onion::llm::PriceSchedule prices{{"1", "0.5", "2"}, {"0.1", "0.05", "0.2"}};
  EXPECT_EQ(onion::llm::meter_cost(1000000, 1000000, 1000000, prices, true, true), "0.35");
  EXPECT_EQ(onion::llm::meter_cost(1000000, 1000000, 1000000, prices, false, true), "3.5");
}

TEST(CircuitBreaker, OpensAfterTwentyConsecutiveTransportFailures) {
  onion::llm::CircuitBreaker breaker;
  for (int index = 0; index < 19; ++index) breaker.on_result(onion::llm::Kind::Transport);
  EXPECT_FALSE(breaker.open());
  breaker.on_result(onion::llm::Kind::Transport);
  EXPECT_TRUE(breaker.open());
  breaker.on_result(onion::llm::Kind::Ok);
  EXPECT_FALSE(breaker.open());
}
TEST(CircuitBreaker, TimeoutsCountAndRateLimitsDoNot) {
  onion::llm::CircuitBreaker breaker;
  for (int index = 0; index < 30; ++index) breaker.on_result(onion::llm::Kind::RateLimited);
  EXPECT_FALSE(breaker.open());
  for (int index = 0; index < 20; ++index) breaker.on_result(onion::llm::Kind::Timeout);
  EXPECT_TRUE(breaker.open());
}
TEST(CircuitBreaker, AuthAndBalanceOpenAfterThreeWithReason) {
  onion::llm::CircuitBreaker auth;
  for (int index = 0; index < 3; ++index) auth.on_result(onion::llm::Kind::AuthError);
  EXPECT_TRUE(auth.open()); EXPECT_EQ(auth.reason(), "authentication");
  onion::llm::CircuitBreaker balance;
  for (int index = 0; index < 3; ++index) balance.on_result(onion::llm::Kind::BalanceError);
  EXPECT_TRUE(balance.open()); EXPECT_EQ(balance.reason(), "balance");
}
}  // namespace
