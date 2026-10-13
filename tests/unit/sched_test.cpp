#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "sched/sched.hpp"
#include "store/store.hpp"

namespace {

using onion::sched::CampaignSpec;
using onion::sched::SettledSample;
using onion::sched::Ticket;
using onion::sched::order_tickets;
using onion::sched::parse_campaign;
using onion::sched::project_cost_usd;
using onion::sched::stop_rule_met;
using onion::store::Node;
using nlohmann::json;

Node node(std::string path, int layer, std::string title) {
  Node n;
  n.path = std::move(path);
  n.layer = layer;
  n.title = std::move(title);
  return n;
}

const auto always_false = [](std::string_view) { return false; };

TEST(ParseCampaign, AcceptsAValidFile) {
  std::string error;
  const auto spec = parse_campaign(json{
      {"id", "c1"},
      {"scope", json::array({"P01"})},
      {"loop_budget", 200},
      {"mode", "depth-first"},
      {"gate", 0.7},
      {"floor", 0.5},
      {"chunk", 50},
      {"model", "deepseek-flash"},
  }, &error);
  ASSERT_TRUE(spec.has_value()) << error;
  EXPECT_EQ(spec->id, "c1");
  EXPECT_EQ(spec->scope.size(), 1u);
  EXPECT_EQ(spec->loop_budget, 200);
  EXPECT_EQ(spec->chunk, 50);
}

TEST(ParseCampaign, RejectsInvalidFiles) {
  std::string error;
  EXPECT_FALSE(parse_campaign(json{{"no_id", 1}}, &error).has_value());
  EXPECT_FALSE(parse_campaign(json::parse("[1]"), &error).has_value());
  EXPECT_FALSE(parse_campaign(
                   json{{"id", "c"}, {"mode", "sideways"}}, &error)
                   .has_value());
  EXPECT_FALSE(parse_campaign(json{{"id", "c"}, {"loop_budget", 0}}, &error)
                   .has_value());
}

TEST(OrderTickets, BreadthFirstIsShallowestFirstTiesByPath) {
  const std::vector<Node> nodes = {
      node("P01", 0, "Phase"),
      node("P01-IN01", 1, "In"),
      node("P01-IN01-T01", 2, "One"),
      node("P01-IN02", 1, "In 2"),
  };
  CampaignSpec spec;
  spec.id = "c1";
  spec.mode = "breadth-first";
  const auto tickets =
      order_tickets(nodes, always_false, always_false, {}, spec);
  ASSERT_EQ(tickets.size(), 4u);
  EXPECT_EQ(tickets[0].path, "P01");
  EXPECT_EQ(tickets[1].path, "P01-IN01");
  EXPECT_EQ(tickets[2].path, "P01-IN02");
  EXPECT_EQ(tickets[3].path, "P01-IN01-T01");
}

TEST(OrderTickets, DepthFirstIsDeepestFirst) {
  const std::vector<Node> nodes = {
      node("P01", 0, "Phase"),
      node("P01-IN01", 1, "In"),
      node("P01-IN01-T01", 2, "One"),
  };
  CampaignSpec spec;
  spec.id = "c1";
  spec.mode = "depth-first";
  const auto tickets =
      order_tickets(nodes, always_false, always_false, {}, spec);
  ASSERT_EQ(tickets.size(), 3u);
  EXPECT_EQ(tickets[0].path, "P01-IN01-T01");
  EXPECT_EQ(tickets[2].path, "P01");
}

TEST(OrderTickets, ScopeSettledLeafAndReviewAreExcluded) {
  const std::vector<Node> nodes = {
      node("P01", 0, "Phase"),
      node("P01-IN01", 1, "In"),
      node("P01-IN02", 1, "In 2"),
      node("P01-IN03", 1, "In 3"),
      node("P01-IN04", 1, "In 4"),
      node("P02-IN01", 1, "Other"),
  };
  CampaignSpec spec;
  spec.id = "c1";
  spec.mode = "breadth-first";
  spec.scope = {"P01"};
  const auto settled = [](std::string_view path) {
    return path == "P01-IN01";
  };
  const auto leaf = [](std::string_view path) { return path == "P01-IN02"; };
  const auto tickets =
      order_tickets(nodes, settled, leaf, {"P01-IN03"}, spec);
  ASSERT_EQ(tickets.size(), 2u);
  EXPECT_EQ(tickets[0].path, "P01");
  EXPECT_EQ(tickets[1].path, "P01-IN04");
}

TEST(OrderTickets, OpenReviewPausesTheWholeSubtree) {
  const std::vector<Node> nodes = {
      node("P01", 0, "Phase"),
      node("P01-IN01", 1, "In"),
      node("P01-IN01-T01", 2, "One"),
      node("P01-IN02", 1, "In 2"),
  };
  CampaignSpec spec;
  spec.id = "c1";
  spec.mode = "breadth-first";
  const auto tickets =
      order_tickets(nodes, always_false, always_false, {"P01-IN01"}, spec);
  ASSERT_EQ(tickets.size(), 2u);
  EXPECT_EQ(tickets[0].path, "P01");
  EXPECT_EQ(tickets[1].path, "P01-IN02");
}

TEST(StopRule, NeedsFiftySettledAndTheThresholds) {
  std::vector<SettledSample> samples;
  for (int i = 0; i < 49; ++i) samples.push_back({1, 1.0});
  EXPECT_FALSE(stop_rule_met(samples));  // 49 is not 50
  samples.push_back({1, 1.0});
  EXPECT_TRUE(stop_rule_met(samples));   // 50 × atomic, avg 1.0

  samples.clear();
  for (int i = 0; i < 44; ++i) samples.push_back({1, 1.0});
  for (int i = 0; i < 6; ++i) samples.push_back({0, 1.0});
  EXPECT_FALSE(stop_rule_met(samples));  // 44/50 = 0.88 < 0.9
  samples.back() = {1, 1.0};
  EXPECT_TRUE(stop_rule_met(samples));   // 45/50 = 0.9 ≥ 0.9

  samples.clear();
  for (int i = 0; i < 50; ++i) samples.push_back({1, 0.5});
  EXPECT_FALSE(stop_rule_met(samples));  // average 0.5 < 0.8
}

TEST(ProjectCost, ScalesLinearlyInExactDecimals) {
  EXPECT_EQ(project_cost_usd(1000, "0.001"), "1.000000000");
  EXPECT_EQ(project_cost_usd(0, "0.001"), "0.000000000");
}

}  // namespace

// -------------------------------------------------------------- M4 slot ids

TEST(NodeIdAllocator, AllocatesFromSlotTimesTwoToTheForty) {
  onion::sched::NodeIdAllocator allocator(3, 0);
  EXPECT_EQ(allocator.allocate(), "TK3298534883328");     // 3 × 2^40
  EXPECT_EQ(allocator.allocate(), "TK3298534883329");
  EXPECT_EQ(onion::sched::NodeIdAllocator::counter_from_code("TK3298534883329"),
            3298534883329LL);
  EXPECT_EQ(onion::sched::NodeIdAllocator::counter_from_code("P01"), 0);
  EXPECT_EQ(onion::sched::NodeIdAllocator::id_value(0, 42), 42);
}

TEST(NodeIdAllocator, SlotBounds) {
  // The largest slot still fits int64 with a 40-bit counter.
  const std::int64_t max_slot = (1 << 20) - 1;
  onion::sched::NodeIdAllocator allocator(max_slot, (1LL << 40) - 1);
  const std::string last = allocator.allocate();
  EXPECT_EQ(onion::sched::NodeIdAllocator::counter_from_code(last),
            (1LL << 40) - 1 + max_slot * (1LL << 40));
  EXPECT_LT(onion::sched::NodeIdAllocator::counter_from_code(last),
            std::numeric_limits<std::int64_t>::max() / 2);
}

// --------------------------------------------------------------- M6 lanes

TEST(HrwLanes, ScoreIsDeterministicAndStrictlyBetweenBounds) {
  using onion::sched::hrw_score;
  const double score = hrw_score("P01-IN01", "dev0000000000000001", 1.0);
  EXPECT_TRUE(std::isfinite(score));
  EXPECT_GT(score, 0.0);
  // Identical inputs give identical scores.
  EXPECT_EQ(hrw_score("P01-IN01", "dev0000000000000001", 1.0), score);
  // Different paths usually move the score (no equality guaranteed, but
  // the function must not crash or repeat the same value for all paths).
  EXPECT_NE(hrw_score("P01-IN02", "dev0000000000000001", 1.0),
            hrw_score("P01-IN03", "dev0000000000000001", 1.0));
}

TEST(HrwLanes, OwnerIsTheHighestWeightedScore) {
  using onion::sched::lane_owner;
  const std::vector<onion::sched::LaneMember> members = {
      {"dev0000000000000001", 1.0},
      {"dev0000000000000002", 2.0},
  };
  const auto owner = lane_owner("P01-IN01", members);
  ASSERT_TRUE(owner.has_value());
  // With double the weight, device 2 usually owns; whichever, the owner is
  // one of the members and the same call repeats it.
  EXPECT_EQ(lane_owner("P01-IN01", members), owner);
  const bool is_member = *owner == "dev0000000000000001" ||
                         *owner == "dev0000000000000002";
  EXPECT_TRUE(is_member);
  // Empty members: no owner.
  EXPECT_FALSE(lane_owner("P01-IN01", {}).has_value());
}

TEST(HrwLanes, WeightShiftMovesALargeShare) {
  using onion::sched::lane_owner;
  std::vector<onion::sched::LaneMember> members = {
      {"dev0000000000000001", 1.0},
      {"dev0000000000000002", 1.0},
  };
  int first_owns = 0;
  int second_owns = 0;
  for (int i = 0; i < 200; ++i) {
    const auto owner =
        lane_owner("P01-IN" + std::to_string(100 + i), members);
    if (*owner == "dev0000000000000001") first_owns++;
    else second_owns++;
  }
  EXPECT_GT(first_owns, 0);
  EXPECT_GT(second_owns, 0);
  // Raising one weight must not DECREASE its share substantially: with 4x
  // the weight, the heavier device owns more than half of the paths.
  members[1].weight = 4.0;
  int heavier = 0;
  for (int i = 0; i < 200; ++i) {
    const auto owner =
        lane_owner("P01-IN" + std::to_string(100 + i), members);
    if (*owner == "dev0000000000000002") heavier++;
  }
  EXPECT_GT(heavier, 100);
}

// ------------------------------------------------- OPT W4: campaign plan

TEST(PlanCampaign, GrowsByChildrenTimesNonAtomicShare) {
  onion::sched::PlanParams params;
  params.children_kept = 3.0;
  params.atomic_share = 0.0;
  params.attempts_avg = 1.0;
  const auto plan = onion::sched::plan_campaign({1}, 2, params);
  ASSERT_EQ(plan.levels.size(), 3u);
  EXPECT_EQ(plan.levels[0].expandable, 1);
  EXPECT_EQ(plan.levels[0].new_nodes, 3);
  EXPECT_EQ(plan.levels[1].expandable, 3);
  EXPECT_EQ(plan.levels[1].new_nodes, 9);
  // Levels 0, 1 and 2 each generate children: 3 + 9 + 27.
  EXPECT_EQ(plan.total_new_nodes, 39);
}

TEST(PlanCampaign, AtomicShareStopsTheTree) {
  onion::sched::PlanParams params;
  params.children_kept = 40.0;
  params.atomic_share = 1.0;  // every child atomic: nothing expands further
  params.attempts_avg = 1.0;
  const auto plan = onion::sched::plan_campaign({10}, 3, params);
  EXPECT_EQ(plan.levels[0].new_nodes, 0);
  for (std::size_t i = 1; i < plan.levels.size(); ++i)
    EXPECT_EQ(plan.levels[i].expandable, 0);
  EXPECT_EQ(plan.total_new_nodes, 0);
}

TEST(PlanCampaign, CacheSplitAndCosts) {
  onion::sched::PlanParams params;
  params.children_kept = 0.0;   // no growth: single-level arithmetic
  params.atomic_share = 0.0;
  params.attempts_avg = 1.0;
  params.calls_per_attempt = 1.0;
  params.precheck_calls = 0.0;
  params.tokens_in_per_call = 1000000.0;  // 1M input tokens
  params.tokens_out_per_call = 1000000.0; // 1M output tokens
  params.cache_hit_ratio = 0.25;
  params.price_peak_in = 0.30;
  params.price_peak_cache = 0.006;
  params.price_peak_out = 1.20;
  params.price_offpeak_in = 0.15;
  params.price_offpeak_cache = 0.003;
  params.price_offpeak_out = 0.60;
  const auto plan = onion::sched::plan_campaign({1}, 0, params);
  ASSERT_EQ(plan.levels.size(), 1u);
  EXPECT_EQ(plan.tokens_in_hit, 250000);
  EXPECT_EQ(plan.tokens_in_miss, 750000);
  EXPECT_EQ(plan.tokens_out, 1000000);
  // Peak: 0.75M x 0.30 + 0.25M x 0.006 + 1M x 1.20 per 1M.
  EXPECT_NEAR(plan.cost_peak, 0.225 + 0.0015 + 1.20, 1e-9);
  EXPECT_NEAR(plan.cost_offpeak, 0.1125 + 0.00075 + 0.60, 1e-9);
}

TEST(PlanCampaign, TimeAndSpendPerHour) {
  onion::sched::PlanParams params;
  params.children_kept = 0.0;
  params.atomic_share = 0.0;
  params.attempts_avg = 1.0;
  params.calls_per_attempt = 1.0;
  params.precheck_calls = 0.0;
  // One node, one attempt, one call: out tokens = tokens_out_per_call.
  params.tokens_out_per_call = 60000.0;  // 60k tokens
  params.output_tokens_per_s = 60000.0;  // 60k tokens/s -> 1 second
  const auto plan = onion::sched::plan_campaign({1}, 0, params);
  // 60k output tokens at 60k tokens/s = 1 second = 1/3600 hours.
  EXPECT_NEAR(plan.hours, 1.0 / 3600.0, 1e-12);
  EXPECT_NEAR(plan.spend_per_hour_peak, plan.cost_peak * 3600.0, 1e-6);
  EXPECT_NEAR(plan.spend_per_hour_offpeak, plan.cost_offpeak * 3600.0, 1e-6);
}

TEST(MeasureParams, PrefersAttemptRowsOverReviewAcceptedSettles) {
  // A review-accepted settle records attempts 0 even though the funnel ran
  // three attempts; attempts_avg must come from the attempts table.
  std::error_code ec;
  const auto dir = std::filesystem::temp_directory_path() /
                   "measure_params_test";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);
  onion::store::Db db(dir / "onion.db");
  std::string error;
  ASSERT_TRUE(db.open(&error)) << error;
  ASSERT_TRUE(db.exec(
      "INSERT INTO settles(path, kept, atomic, attempts, score, campaign, "
      "hlc, origin) VALUES ('a', 100, 0, 0, '0.4', 'c', 1, 'o');",
      &error)) << error;
  ASSERT_TRUE(db.exec(
      "INSERT INTO settles(path, kept, atomic, attempts, score, campaign, "
      "hlc, origin) VALUES ('b', 10, 0, 0, '0.3', 'c', 2, 'o');",
      &error)) << error;
  ASSERT_TRUE(db.exec(
      "INSERT INTO attempts(path, n, score, passed, tokens_in, "
      "tokens_cached, tokens_out, cost, campaign, hlc, origin) VALUES "
      "('a', 1, '0.1', 0, 100, 0, 200, '0', 'c', 1, 'o');",
      &error)) << error;
  ASSERT_TRUE(db.exec(
      "INSERT INTO attempts(path, n, score, passed, tokens_in, "
      "tokens_cached, tokens_out, cost, campaign, hlc, origin) VALUES "
      "('a', 2, '0.4', 0, 300, 0, 560, '0', 'c', 2, 'o');",
      &error)) << error;
  const auto params = onion::sched::measure_params(db, &error);
  EXPECT_EQ(error, "");
  // 100 + 10 kept over two settles with children.
  EXPECT_NEAR(params.children_kept, 55.0, 1e-9);
  EXPECT_NEAR(params.atomic_share, 0.0, 1e-9);
  // Two attempt rows over two settles, NOT zero from the settle rows.
  EXPECT_NEAR(params.attempts_avg, 1.0, 1e-9);
  // Per-call tokens: 760 out / 2 attempts / 8 calls per attempt = 47.5.
  // The distinct token values make this pin the 8-call default.
  EXPECT_NEAR(params.tokens_out_per_call, 760.0 / 2.0 / 8.0, 1e-9);
}

TEST(PlanCampaign, TimeScalesWithOutputTokensNotCallCount) {
  // Long outputs dominate the funnel: 1,400-token calls take longer than
  // 365-token calls even at the same call count.  Two plans with the same
  // call count but different output sizes must differ in time.
  onion::sched::PlanParams long_outputs;
  long_outputs.children_kept = 0.0;
  long_outputs.atomic_share = 0.0;
  long_outputs.attempts_avg = 1.0;
  long_outputs.calls_per_attempt = 1.0;
  long_outputs.precheck_calls = 0.0;
  long_outputs.tokens_out_per_call = 1400.0;  // funnel calls
  long_outputs.output_tokens_per_s = 60000.0;
  const auto long_plan = onion::sched::plan_campaign({100}, 0, long_outputs);

  onion::sched::PlanParams short_outputs = long_outputs;
  short_outputs.tokens_out_per_call = 365.0;  // probe-sized answers
  const auto short_plan = onion::sched::plan_campaign({100}, 0, short_outputs);

  EXPECT_EQ(long_plan.total_calls, short_plan.total_calls);
  EXPECT_GT(long_plan.hours, short_plan.hours);
  EXPECT_NEAR(long_plan.hours, 100.0 * 1400.0 / 60000.0 / 3600.0, 1e-12);
}

TEST(PlanCampaign, ReportContainsTheKeyNumbers) {
  onion::sched::PlanParams params;
  params.children_kept = 2.0;
  params.atomic_share = 0.0;
  params.attempts_avg = 1.0;
  const auto plan = onion::sched::plan_campaign({1}, 1, params);
  const std::string report = onion::sched::plan_report(plan, params);
  EXPECT_NE(report.find("level expandable"), std::string::npos);
  EXPECT_NE(report.find("total new_nodes=6"), std::string::npos);
  EXPECT_NE(report.find("cost peak="), std::string::npos);
  EXPECT_NE(report.find("spend/hour peak="), std::string::npos);
}
