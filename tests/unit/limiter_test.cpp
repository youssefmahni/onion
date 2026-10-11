#include "llm/llm.hpp"

#include <gtest/gtest.h>

namespace {
// Windows are built with window_length_s = 1.0, so the window goodput equals
// completed_out_tokens.  `ok` models a utilization-bound provider: output
// tokens scale with the limit used.
onion::llm::LimitWindow ok(std::uint64_t out_tokens, double median_s = 1.0,
                           double p95_s = 1.0, std::size_t calls = 100,
                           std::uint64_t ended_at_s = 0) {
  onion::llm::LimitWindow window;
  window.ended_at_s = ended_at_s;
  window.calls = calls;
  window.completed_out_tokens = out_tokens;
  window.median_latency_s = median_s;
  window.p95_latency_s = p95_s;
  return window;
}

onion::llm::LimitWindow pressured(std::size_t rate_limited, std::size_t server_errors,
                                  std::size_t timeouts, std::size_t connect_failures = 0) {
  onion::llm::LimitWindow window = ok(0);
  window.rate_limited = rate_limited;
  window.server_errors = server_errors;
  window.timeouts = timeouts;
  window.connect_failures = connect_failures;
  return window;
}

// Simulated provider: throughput caps at N in flight and the p95 doubles by
// the time the limit is 10% past the cap (the sharp saturation the
// `plateau_flat_median` fake-server scenario models).
void sim_window(onion::llm::Limiter& limiter, int n, std::uint64_t t) {
  const int used = limiter.begin_window(t);
  const auto out = static_cast<std::uint64_t>(std::min(used, n)) * 40;
  const double p95 = 1.0 + 10.0 * static_cast<double>(std::max(0, used - n)) / n;
  limiter.on_window(ok(out, 1.0, p95, static_cast<std::size_t>(used), t + 1));
}

void sim_run(onion::llm::Limiter& limiter, int n, std::uint64_t from, std::uint64_t to) {
  for (std::uint64_t t = from; t < to; ++t) sim_window(limiter, n, t);
}

TEST(Limiter, GoodputSignalIsAnEwmaOverTwoWindows) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(100));
  EXPECT_DOUBLE_EQ(limiter.goodput_signal(), 100.0);
  limiter.on_window(ok(200));
  EXPECT_DOUBLE_EQ(limiter.goodput_signal(), 150.0);
  limiter.on_window(ok(400));
  EXPECT_DOUBLE_EQ(limiter.goodput_signal(), 275.0);
}

TEST(Limiter, MultipliesWhileGoodputRisesFivePercent) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160));  // anchors the baseline and performs the first
  EXPECT_EQ(limiter.limit(), 20);  // unassessed increase
  limiter.on_window(ok(200));  // G rises 25% -> assessed increase passes
  EXPECT_EQ(limiter.limit(), 25);
  limiter.on_window(ok(250));
  EXPECT_EQ(limiter.limit(), 31);
  limiter.on_window(ok(312));
  EXPECT_EQ(limiter.limit(), 38);
  limiter.on_window(ok(390));
  EXPECT_EQ(limiter.limit(), 47);
}

TEST(Limiter, RevertsToThePreviousLimitWhenGoodputDoesNotRise) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160));  // anchor + first increase to 20 (G = 160)
  EXPECT_EQ(limiter.limit(), 20);
  limiter.on_window(ok(160));  // G stays 160 < 168: assessment fails
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(ok(160));  // hold one
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(ok(160));  // hold two
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(ok(160));  // plateau window: still no increase
  EXPECT_EQ(limiter.limit(), 16);
}

TEST(Limiter, PlateauProbeAdoptsWhenCapacityGrew) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160));  // anchor + increase to 20
  limiter.on_window(ok(160));  // flat goodput: revert to 16, hold two windows
  limiter.on_window(ok(160));  // hold 1
  limiter.on_window(ok(160));  // hold 2
  for (int i = 0; i < 10; ++i) limiter.on_window(ok(160));  // 10 plateau windows
  EXPECT_EQ(limiter.limit(), 17);  // probe window starts at 16 x 1.1
  // The provider's capacity grew: at the probed limit the goodput rises more
  // than enough to move the signal 5% above the plateau entry signal.
  limiter.on_window(ok(340));
  EXPECT_EQ(limiter.limit(), 17);  // adopted the probed limit
  limiter.on_window(ok(340));      // the ramp resumes with the next window
  EXPECT_EQ(limiter.limit(), 21);  // unassessed increase x1.25
}

TEST(Limiter, PlateauProbeReturnsAndRestartsTheCounter) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160));  // anchor + increase to 20
  limiter.on_window(ok(160));  // revert to 16, hold two
  limiter.on_window(ok(160));
  limiter.on_window(ok(160));
  for (int i = 0; i < 10; ++i) limiter.on_window(ok(160));
  EXPECT_EQ(limiter.limit(), 17);  // probe window
  limiter.on_window(ok(160));      // capacity unchanged: probe fails
  EXPECT_EQ(limiter.limit(), 16);
  for (int i = 0; i < 9; ++i) limiter.on_window(ok(160));
  EXPECT_EQ(limiter.limit(), 16);  // still counting, no probe yet
}

TEST(Limiter, TailGuardStepsBackAndDoesNotBlindlyReIncrease) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160, 1.0, 1.0));  // anchor p95 baseline = 1.0, limit 20
  EXPECT_EQ(limiter.limit(), 20);
  limiter.on_window(ok(200, 1.0, 2.2));  // p95 > 2 x baseline
  EXPECT_EQ(limiter.limit(), 18);        // 20 x 0.9
  limiter.on_window(ok(200, 1.0, 2.2));  // guard fires during the hold too
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(ok(200, 1.0, 1.0));  // hold window after the last guard
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(ok(200, 1.0, 1.0));  // recovered: plateau, no blind increase
  EXPECT_EQ(limiter.limit(), 16);
}

TEST(Limiter, TailGuardNeedsTwiceTheBaselineP95) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160, 1.0, 1.0));  // anchor + increase to 20
  EXPECT_EQ(limiter.limit(), 20);
  limiter.on_window(ok(200, 1.0, 1.9));  // below the threshold: ramp proceeds
  EXPECT_EQ(limiter.limit(), 25);
}

TEST(Limiter, TailGuardHoldPassesBeforePlateauCountingResumes) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160, 1.0, 1.0));  // anchor + increase to 20
  limiter.on_window(ok(200, 1.0, 2.2));  // guard: 18, hold 1, plateau
  limiter.on_window(ok(200, 1.0, 1.0));  // recovered: the hold window passes,
  EXPECT_EQ(limiter.limit(), 18);        // no plateau counting yet
  limiter.on_window(ok(200, 1.0, 1.0));  // first plateau window
  EXPECT_EQ(limiter.limit(), 18);
  // The plateau counter restarts from the guard, so the next probe fires only
  // after ten counted windows — not before.
  for (int i = 0; i < 8; ++i) limiter.on_window(ok(200, 1.0, 1.0));
  EXPECT_EQ(limiter.limit(), 18);  // nine counted windows: no probe yet
}

TEST(Limiter, HalvesOnRateLimitAndHoldsOneWindow) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160));  // anchor + increase to 20
  limiter.on_window(pressured(1, 0, 0));
  EXPECT_EQ(limiter.limit(), 10);
  limiter.on_window(ok(80));  // hold window
  EXPECT_EQ(limiter.limit(), 10);
  limiter.on_window(ok(80));  // recovery: unassessed increase
  EXPECT_EQ(limiter.limit(), 12);
}

TEST(Limiter, HalvesOnServerError) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(pressured(0, 1, 0));
  EXPECT_EQ(limiter.limit(), 8);
}

TEST(Limiter, HalvesOnlyWhenTimeoutsExceedFivePercent) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(pressured(0, 0, 5));  // exactly 5%: not above the threshold
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(pressured(0, 0, 6));
  EXPECT_EQ(limiter.limit(), 8);
}

TEST(Limiter, HalvesOnlyWhenConnectFailuresExceedTwoPercent) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(pressured(0, 0, 0, 2));  // exactly 2%: not above
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(pressured(0, 0, 0, 3));
  EXPECT_EQ(limiter.limit(), 8);
}

TEST(Limiter, NeverExceedsCeilingAndNeverFallsBelowOne) {
  onion::llm::Limiter limiter(15, 16, 1.0);
  limiter.on_window(ok(150));  // anchor + increase to the ceiling
  EXPECT_EQ(limiter.limit(), 16);
  limiter.on_window(ok(300));  // assessment passes; the increase is a no-op
  EXPECT_EQ(limiter.limit(), 16);  // at the ceiling: hold, no revert
  for (int i = 0; i < 15; ++i) {
    limiter.on_window(ok(300));
    EXPECT_LE(limiter.limit(), 16);
    EXPECT_GE(limiter.limit(), 1);
  }
  EXPECT_EQ(limiter.limit(), 16);  // plateau at the ceiling never probes

  onion::llm::Limiter minimum(1, 16, 1.0);
  minimum.on_window(pressured(1, 0, 0));
  EXPECT_EQ(minimum.limit(), 1);
}

TEST(Limiter, FreshLimiterReportsClampedProfileStartingValue) {
  EXPECT_EQ(onion::llm::Limiter(16, 2000).limit(), 16);
  EXPECT_EQ(onion::llm::Limiter(0, 16).limit(), 1);
  EXPECT_EQ(onion::llm::Limiter(32, 16).limit(), 16);
}

TEST(Limiter, HourlyProbeCapsRestoresAndRecordsTheP95Baseline) {
  onion::llm::Limiter limiter(16, 2000, 1.0);
  limiter.on_window(ok(160, 0.2, 0.3));  // anchor + increase to 20
  EXPECT_EQ(limiter.low_load_baseline_s(), 0.2);
  EXPECT_EQ(limiter.low_load_baseline_p95_s(), 0.3);
  EXPECT_EQ(limiter.limit(), 20);
  EXPECT_EQ(limiter.begin_window(3600), 16);
  limiter.on_window(ok(160, 0.4, 0.5, 100, 3630));
  EXPECT_EQ(limiter.limit(), 20);
  EXPECT_EQ(limiter.low_load_baseline_s(), 0.4);
  EXPECT_EQ(limiter.low_load_baseline_p95_s(), 0.5);
}

TEST(Limiter, SettlesWithinTenPercentOfThePlateauAndStays) {
  constexpr int n = 1000;
  onion::llm::Limiter limiter(400, 2000, 1.0);
  sim_run(limiter, n, 0, 7 * 3600);
  // The last simulated hour must be settled within 10% of the plateau.
  bool settled = true;
  for (std::uint64_t t = 7 * 3600; t < 8 * 3600; ++t) {
    sim_window(limiter, n, t);
    ASSERT_LE(limiter.limit(), 2000);
    if (std::abs(limiter.limit() - n) > n / 10) settled = false;
  }
  EXPECT_TRUE(settled);
}

TEST(Limiter, ReachesNinetyPercentOfThePlateauWithinThreeMinutes) {
  // Start 400, plateau 1,000: four x1.25 steps reach 977 in four windows
  // (two simulated minutes), each backed by rising goodput.
  onion::llm::Limiter limiter(400, 2000, 1.0);
  sim_run(limiter, 1000, 0, 6);
  EXPECT_GE(limiter.limit(), 900);
}

TEST(Limiter, RecoversFromTheFloorAfterAHardBackoff) {
  // A backoff storm can push the limit to 1; the multiplicative step must
  // still grow it back (floor(1 x 1.25) would lock it there forever).
  onion::llm::Limiter limiter(16, 2000, 1.0);
  for (int i = 0; i < 5; ++i) limiter.on_window(pressured(1, 0, 0));
  EXPECT_EQ(limiter.limit(), 1);
  limiter.on_window(ok(10));  // hold window
  EXPECT_EQ(limiter.limit(), 1);
  limiter.on_window(ok(10));  // recovery: unassessed increase
  EXPECT_EQ(limiter.limit(), 2);
  limiter.on_window(ok(20));  // rising goodput: assessed increase passes
  EXPECT_EQ(limiter.limit(), 3);
  limiter.on_window(ok(30));
  EXPECT_EQ(limiter.limit(), 4);
}

TEST(Limiter, BacksOffWithinOneWindowOnPressureAndDescendsOnCapacityLoss) {
  // Capacity drops from 1,000 to 400 mid-run: the p95 blows past 2x and the
  // tail guard descends while the ramp stays out of the way.
  onion::llm::Limiter limiter(400, 2000, 1.0);
  sim_run(limiter, 1000, 0, 40);  // warm up to the plateau at 1,000
  const int before = limiter.limit();
  ASSERT_GE(before, 500);
  // Capacity 400 from here on.
  for (std::uint64_t t = 40; t < 200; ++t) {
    const int used = limiter.begin_window(t);
    const auto out = static_cast<std::uint64_t>(std::min(used, 400)) * 40;
    const double p95 = 1.0 + 2.0 * static_cast<double>(std::max(0, used - 400)) / 400;
    limiter.on_window(ok(out, 1.0, p95, static_cast<std::size_t>(used), t + 1));
  }
  // The limit must have descended towards the new capacity and must not
  // exceed it by more than 50% (the guard holds it at p95 <= 2x baseline,
  // which in this model means in flight <= 1.5 x 400 = 600).
  EXPECT_LE(limiter.limit(), 600);
}
}  // namespace
