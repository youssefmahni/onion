#include <gtest/gtest.h>

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
