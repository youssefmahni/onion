#include "llm/llm.hpp"

#include <atomic>
#include <chrono>
#include <future>
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
TEST(Budget, TimeoutKeepsFullReservationAsUncertainSpend) {
  onion::llm::Budget budget("1.00");
  auto reservation = budget.reserve("0.80");
  ASSERT_TRUE(reservation);
  budget.settle(*reservation, reservation->reserved_usd);
  EXPECT_EQ(budget.spent_usd(), "0.8");
  EXPECT_EQ(budget.remaining_usd(), "0.2");
}

TEST(Metering, UsesOffpeakOnlyForCallsInsideOneWindow) {
  onion::llm::PriceSchedule prices{{"1", "0.5", "2"}, {"0.1", "0.05", "0.2"}};
  EXPECT_EQ(onion::llm::meter_cost(1000000, 1000000, 1000000, prices, true, true), "0.35");
  EXPECT_EQ(onion::llm::meter_cost(1000000, 1000000, 1000000, prices, false, true), "3.5");
}

TEST(Metering, SpanningCallBillsTheHigherTriplet) {
  // A call spanning a price-window boundary bills at the higher triplet
  // (decision 0004): the flag pair means "started in off-peak" but
  // "request did not stay in one window".
  onion::llm::PriceSchedule prices{{"1", "0.5", "2"}, {"0.1", "0.05", "0.2"}};
  EXPECT_EQ(onion::llm::meter_cost(1000000, 1000000, 1000000, prices, false, true), "3.5");
  EXPECT_EQ(onion::llm::meter_cost(1000000, 1000000, 1000000, prices, false, false), "3.5");
  EXPECT_EQ(onion::llm::meter_cost(1000000, 1000000, 1000000, prices, true, true), "0.35");
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

TEST(ClientHygiene, ResolverMemoExpiresPerTtl) {
  // The resolver hook memoizes per host for the TTL.  The requests fail at
  // connect (nothing listens on port 1) but the hook count is what matters.
  onion::llm::Client::Options options;
  options.base_url = "http://127.0.0.1:1";
  options.timeout_s = 5;
  options.connect_timeout_s = 5;
  options.max_in_flight = 4;
  options.dns_cache_ttl_s = 1;
  std::atomic<int> calls{0};
  options.dns_resolver = [&calls](const std::string&, std::uint16_t) {
    ++calls;
    return "127.0.0.1";
  };
  onion::llm::Client client(std::move(options));
  const auto run_one = [&] {
    std::promise<void> completed;
    auto future = completed.get_future();
    client.submit({"m", "", "hi", 8, 0.0},
                  [&](onion::llm::Result) { completed.set_value(); });
    future.wait();
  };
  run_one();
  EXPECT_EQ(client.dns_lookups(), 1);
  run_one();  // inside the TTL: memoized, no fresh lookup
  EXPECT_EQ(client.dns_lookups(), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  run_one();  // TTL expired: one fresh lookup
  EXPECT_EQ(client.dns_lookups(), 2);
  run_one();  // memoized again
  EXPECT_EQ(client.dns_lookups(), 2);
}
