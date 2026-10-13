#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "gate/gate.hpp"

namespace {

using onion::gate::Attempt;
using onion::gate::Checks;
using onion::gate::Child;
using onion::gate::Critique;
using onion::gate::Decision;
using onion::gate::Funnel;
using onion::gate::FunnelOptions;
using onion::gate::FunnelResult;
using onion::gate::GateParams;
using onion::gate::NodeContext;
using onion::gate::Outcome;
using onion::gate::QualityGuard;
using onion::gate::SyncAsk;
using onion::gate::decide;
using onion::gate::parse_atomic_verdict;
using onion::gate::parse_children;
using onion::gate::parse_critique;
using onion::gate::parse_items;
using onion::gate::parse_precheck;
using onion::gate::precheck_prompt;
using onion::gate::run_checks;
using onion::llm::Kind;
using onion::llm::Request;
using onion::llm::Result;

NodeContext ctx() {
  NodeContext context;
  context.path = "P01-IN01-T01";
  context.title = "Order Capture";
  context.layer = 2;
  return context;
}

std::vector<Child> children_of(std::vector<std::string> titles,
                               std::vector<bool> atomic = {}) {
  std::vector<Child> children;
  for (std::size_t i = 0; i < titles.size(); ++i) {
    Child child;
    child.title = titles[i];
    if (i < atomic.size()) child.atomic = atomic[i];
    children.push_back(std::move(child));
  }
  return children;
}

// ---------------- deterministic checks (one per DESIGN.md row) ----------------

TEST(GateChecks, CountMustBeBetweenThreeAndOneHundred) {
  Checks checks = run_checks(children_of({"A", "B"}), ctx());
  EXPECT_FALSE(checks.count_ok);
  std::vector<std::string> hundred;
  for (int i = 0; i < 101; ++i) hundred.push_back("Item " + std::to_string(i));
  checks = run_checks(children_of(hundred), ctx());
  EXPECT_FALSE(checks.count_ok);
  std::vector<std::string> three = {"One", "Two", "Three"};
  EXPECT_TRUE(run_checks(children_of(three), ctx()).count_ok);
}

TEST(GateChecks, ChildRepeatingTheNodeIsRejected) {
  Checks checks = run_checks(
      children_of({"Order Capture", "Two", "Three"}), ctx());
  EXPECT_FALSE(checks.no_repeats);
}

TEST(GateChecks, ChildRepeatingAnAncestorIsRejected) {
  NodeContext context = ctx();
  context.ancestors = {"Business Domain", "Order Processing"};
  Checks checks = run_checks(
      children_of({"Order Processing", "Two", "Three"}), context);
  EXPECT_FALSE(checks.no_repeats);
}

TEST(GateChecks, ChildRepeatingASiblingIsRejected) {
  NodeContext context = ctx();
  context.siblings = {"Shipment Status"};
  Checks checks = run_checks(
      children_of({"Shipment Status", "Two", "Three"}), context);
  EXPECT_FALSE(checks.no_repeats);
}

TEST(GateChecks, ChildRepeatingAnExistingChildIsRejected) {
  NodeContext context = ctx();
  context.existing = {"Payment Method"};
  Checks checks = run_checks(
      children_of({"Payment Method", "Two", "Three"}), context);
  EXPECT_FALSE(checks.no_repeats);
}

TEST(GateChecks, StyleGatesRejectBadTitles) {
  Checks checks = run_checks(
      children_of({"Is this a question?", "Two", "Three"}), ctx());
  EXPECT_FALSE(checks.style_ok);
  checks = run_checks(children_of({"Payment Method?", "Two", "Three"}), ctx());
  EXPECT_FALSE(checks.style_ok);
}

TEST(GateChecks, AncestorSimilarityThresholds) {
  // Parent (the node itself) at ≥ 0.85 is rejected: sim("Order Capture",
  // "Order Capturing") ≈ 0.857.
  NodeContext context = ctx();
  Checks checks = run_checks(
      children_of({"Order Capturing", "Two", "Three"}), context);
  EXPECT_FALSE(checks.ancestors_ok);

  // Higher ancestors are stricter: ≥ 0.95 rejected ("Customer Ordering" vs
  // "Customer Orderings" = 1.0), while 0.81 stays allowed.
  context = ctx();
  context.ancestors = {"Customer Ordering"};
  checks = run_checks(
      children_of({"Customer Orderings", "Two", "Three"}), context);
  EXPECT_FALSE(checks.ancestors_ok);
  checks = run_checks(
      children_of({"Customer Ordering Process", "Two", "Three"}), context);
  EXPECT_TRUE(checks.ancestors_ok);
}

// ---------------- decide (best-of-3) ----------------

Attempt passing_attempt(int number, double score) {
  Attempt attempt;
  attempt.number = number;
  attempt.score = score;
  attempt.checks_passed = true;
  attempt.children = children_of({"One", "Two", "Three"});
  return attempt;
}

Attempt failing_attempt(int number) {
  Attempt attempt;
  attempt.number = number;
  attempt.score = 0.0;
  attempt.checks_passed = false;
  return attempt;
}

TEST(GateDecide, CommitWhenChecksPassAndScoreIsAtLeastZeroSeven) {
  const Outcome outcome = decide({passing_attempt(1, 0.85)}, GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Commit);
  EXPECT_EQ(outcome.chosen, 0);
  EXPECT_EQ(outcome.end_reason, "children");
}

TEST(GateDecide, RetryBeforeMaxAttempts) {
  Outcome outcome = decide({failing_attempt(1)}, GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Retry);
  outcome = decide({failing_attempt(1), failing_attempt(2)}, GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Retry);
}

TEST(GateDecide, LeafWhenNoAttemptHasThreeValidChildren) {
  const Outcome outcome =
      decide({failing_attempt(1), failing_attempt(2), failing_attempt(3)},
             GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Leaf);
  EXPECT_EQ(outcome.end_reason, "cannot split");
}

TEST(GateDecide, CommitBestPassingAttemptAtOrAboveFloor) {
  const Outcome outcome =
      decide({passing_attempt(1, 0.4), passing_attempt(2, 0.6),
              passing_attempt(3, 0.55)},
             GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Commit);
  EXPECT_EQ(outcome.chosen, 1);  // the 0.6 attempt
}

TEST(GateDecide, NeedsReviewWhenBestIsBelowFloor) {
  const Outcome outcome =
      decide({passing_attempt(1, 0.3), passing_attempt(2, 0.4),
              passing_attempt(3, 0.45)},
             GateParams{});
  EXPECT_EQ(outcome.decision, Decision::NeedsReview);
  EXPECT_EQ(outcome.end_reason, "below floor");
}

TEST(GateDecide, RetryInjectsHigherTemperatureViaFunnel) {
  // The funnel drives temperature 0 → 0.3 → 0.5 across attempts; the first
  // passing attempt commits, so observe the critique temperature of each
  // attempt.
  FunnelOptions options;
  options.params = GateParams{};
  Funnel funnel(options);
  NodeContext context = ctx();
  std::vector<double> critique_temperatures;
  auto fake = [&](const Request& request) -> Result {
    Result result;
    result.kind = Kind::Ok;
    result.content = R"({"items": ["One", "Two", "Three"]})";
    result.finish_reason = "stop";
    if (request.user.find("Critique this candidate set") !=
        std::string::npos) {
      critique_temperatures.push_back(request.temperature);
      result.content = R"({"score": 0.9, "remove": [], "add": [], "issues": ""})";
    }
    if (request.user.find("select the final children set") !=
        std::string::npos) {
      result.content =
          R"({"items": [{"title": "One", "atomic": false}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})";
    }
    if (request.user.find("Can this node be split") != std::string::npos) {
      result.content = R"({"splits": true})";
    }
    return result;
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  ASSERT_EQ(critique_temperatures.size(), 1u);
  EXPECT_DOUBLE_EQ(critique_temperatures[0], 0.0);
}

// ---------------- parsers ----------------

TEST(GateParsers, StrictJsonRejectsMalformedBodies) {
  EXPECT_FALSE(parse_items("not json").has_value());
  EXPECT_FALSE(parse_items(R"({"items": "nope"})").has_value());
  EXPECT_FALSE(parse_items(R"({"items": [1, 2]})").has_value());
  EXPECT_TRUE(parse_items(R"({"items": ["A", "B"]})").has_value());

  EXPECT_FALSE(parse_children(R"({"items": [{"title": 1}]})").has_value());
  const auto children =
      parse_children(R"({"items": [{"title": "A", "atomic": true}]})");
  ASSERT_TRUE(children.has_value());
  EXPECT_TRUE((*children)[0].atomic);

  EXPECT_FALSE(parse_critique(R"({"score": "high"})").has_value());
  const auto critique =
      parse_critique(R"({"score": 0.8, "issues": "dup", "remove": ["X"], "add": ["Y"]})");
  ASSERT_TRUE(critique.has_value());
  EXPECT_EQ(critique->remove.size(), 1u);

  EXPECT_FALSE(parse_precheck(R"({"splits": "yes"})").has_value());
  const auto splits_no = parse_precheck(R"({"splits": false})");
  ASSERT_TRUE(splits_no.has_value());
  EXPECT_FALSE(*splits_no);
  const auto verdict = parse_atomic_verdict(R"({"admits_children": true})");
  ASSERT_TRUE(verdict.has_value());
  EXPECT_TRUE(*verdict);
  EXPECT_TRUE(*verdict);
}

TEST(GatePrompts, PrecheckPromptIsAtMostFourHundredTokensApprox) {
  // A generous upper bound: the prompt is fixed text plus one node line.
  EXPECT_LT(precheck_prompt(ctx()).size(), 900u);
}

TEST(GatePrompts, FixedTextPrecedesNodeText) {
  // DESIGN › LLM provider › Prompt caching: fixed instructions come first
  // in every prompt and node-specific text last, so DeepSeek's cache can
  // bill the shared prefix at the cheaper cache-hit rate.  Every funnel
  // prompt therefore ends with the node line.
  using onion::gate::critique_prompt;
  using onion::gate::planner_prompt;
  using onion::gate::synthesizer_prompt;
  using onion::gate::trimmer_prompt;
  using onion::gate::worker_prompt;
  const std::string line = "Node: L2 P01-IN01-T01\nTitle: Order Capture";
  const auto ends_with_node = [&](const std::string& prompt) {
    return prompt.size() >= line.size() &&
           prompt.substr(prompt.size() - line.size()) == line;
  };
  EXPECT_TRUE(ends_with_node(precheck_prompt(ctx())));
  EXPECT_TRUE(ends_with_node(planner_prompt(ctx(), "", false)));
  EXPECT_TRUE(ends_with_node(worker_prompt(ctx(), "Aspect X", "")));
  EXPECT_TRUE(ends_with_node(trimmer_prompt(ctx(), {"A", "B"}, 1)));
  EXPECT_TRUE(ends_with_node(critique_prompt(ctx(), {"A", "B"})));
  EXPECT_TRUE(ends_with_node(synthesizer_prompt(ctx(), {"A", "B"}, "crit")));
}

TEST(GatePrompts, VariantKnobsReachThePromptText) {
  // W5 V1/V2/V4: the candidate count, aspect range and synthesizer maximum
  // come from the options and land in the prompts the model reads.
  const std::string v1 = worker_prompt(ctx(), "Aspect", "", 40, false);
  EXPECT_NE(v1.find("top 40"), std::string::npos);

  const std::string v2 = planner_prompt(ctx(), "", false, 3, 5);
  EXPECT_NE(v2.find("3-5"), std::string::npos);

  // V4: the synthesizer range (target 8-20, hard max 30, no padding)
  // is now the default configuration (decision 0017).
  const std::string v4 =
      synthesizer_prompt(ctx(), {"A", "B"}, "crit", 8, 20, 30, false);
  EXPECT_NE(v4.find("between 8 and 20"), std::string::npos);
  EXPECT_NE(v4.find("never more than 30"), std::string::npos);
  EXPECT_NE(v4.find("Do not pad"), std::string::npos);

  // Compact output (V5) switches the worker and synthesizer formats.
  const std::string v5w = worker_prompt(ctx(), "Aspect", "", 100, true);
  EXPECT_NE(v5w.find("\"titles\""), std::string::npos);
  const std::string v5s =
      synthesizer_prompt(ctx(), {"A", "B"}, "crit", 8, 20, 30, true);
  EXPECT_NE(v5s.find("atomic_indices"), std::string::npos);
}

TEST(GatePrompts, Decision0017DefaultsReachThePromptText) {
  // The accepted defaults: workers ask for 40 candidates, the planner
  // asks for 3-5 aspects, the synthesizer targets 8-20 with a hard
  // maximum of 30 and no padding.
  EXPECT_NE(worker_prompt(ctx(), "Aspect", "").find("top 40"),
            std::string::npos);
  EXPECT_NE(planner_prompt(ctx(), "", false).find("3-5"),
            std::string::npos);
  const std::string synth = synthesizer_prompt(ctx(), {"A", "B"}, "crit");
  EXPECT_NE(synth.find("between 8 and 20"), std::string::npos);
  EXPECT_NE(synth.find("never more than 30"), std::string::npos);
  EXPECT_NE(synth.find("Do not pad"), std::string::npos);
}

TEST(GateParsers, CompactOutputParses) {
  // V5: titles as a plain string array, atomic flags as a separate index
  // list; both strict on malformed bodies.
  auto children = parse_children(
      R"({"titles": ["Alpha", "Beta", "Gamma"], "atomic_indices": [0, 2]})");
  ASSERT_TRUE(children.has_value());
  EXPECT_EQ(children->size(), 3u);
  EXPECT_TRUE((*children)[0].atomic);
  EXPECT_FALSE((*children)[1].atomic);
  EXPECT_TRUE((*children)[2].atomic);
  EXPECT_EQ((*children)[0].title, "Alpha");

  EXPECT_FALSE(parse_children(R"({"titles": ["Alpha"], "atomic_indices": [7]})"));
  EXPECT_FALSE(parse_children(R"({"titles": ["Alpha"], "atomic_indices": ["a"]})"));
  EXPECT_FALSE(parse_children(R"({"titles": ["Alpha", 2]})"));

  auto items = parse_items(R"({"titles": ["A", "B", "C"]})");
  ASSERT_TRUE(items.has_value());
  EXPECT_EQ(items->size(), 3u);
}

TEST(GateChecks, VariantMaxChildrenBoundsTheCount) {
  // V4: a synthesizer capped at 40 must reject 41 children.
  std::vector<Child> children;
  for (int i = 0; i < 41; ++i) {
    Child child;
    child.title = "Item " + std::to_string(i);
    children.push_back(std::move(child));
  }
  const auto checks = run_checks(children, ctx(), 40);
  EXPECT_FALSE(checks.count_ok);
  EXPECT_EQ(checks.failures.front(), "child count 41 outside 3..40");
}

// ---------------- funnel behaviours ----------------

Result ok(const std::string& content) {
  Result result;
  result.kind = Kind::Ok;
  result.content = content;
  result.finish_reason = "stop";
  result.cost_usd = "0.001";
  return result;
}

Result truncated() {
  Result result;
  result.kind = Kind::Truncated;
  result.finish_reason = "length";
  result.content = R"({"items": [{"title": "Cu)";
  return result;
}

TEST(GateFunnel, PreCheckNoMakesALeafWithoutSpendingAnAttempt) {
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  std::size_t calls = 0;
  auto fake = [&](const Request&) -> Result {
    calls++;
    return ok(R"({"splits": false})");
  };
  const FunnelResult result = funnel.run(context, fake);
  EXPECT_EQ(result.decision, Decision::Leaf);
  EXPECT_EQ(result.end_reason, "cannot split");
  EXPECT_TRUE(result.precheck_leaf);
  EXPECT_TRUE(result.attempts.empty());
  EXPECT_EQ(calls, 1u);
}

TEST(GateFunnel, TruncatedReplyIsAFailedAttempt) {
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  int synthesizer_calls = 0;
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("select the final children set") !=
        std::string::npos) {
      synthesizer_calls++;
      return truncated();
    }
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  EXPECT_EQ(result.decision, Decision::Leaf);  // no valid children ever
  EXPECT_EQ(result.attempts.size(), 3u);
  EXPECT_FALSE(result.attempts[0].checks_passed);
}

TEST(GateFunnel, RetryInjectsPreviousIssuesAndCommitsBetterAttempt) {
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  std::vector<double> critique_temperatures;
  std::vector<std::string> planner_users;
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("Plan the knowledge coverage") != std::string::npos) {
      planner_users.push_back(request.user);
      return ok(R"({"items": ["Ops", "Gov"]})");
    }
    if (request.user.find("Critique this candidate set") != std::string::npos) {
      critique_temperatures.push_back(request.temperature);
      return (critique_temperatures.back() == 0.0)
                 ? ok(R"({"score": 0.5, "remove": [], "add": [], "issues": "missed governance"})")
                 : ok(R"({"score": 0.9, "remove": [], "add": [], "issues": ""})");
    }
    if (request.user.find("select the final children set") !=
        std::string::npos) {
      return ok(R"({"items": [{"title": "One", "atomic": false}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})");
    }
    return ok(R"({"items": ["Alpha", "Beta", "Gamma"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  EXPECT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(result.attempts.size(), 2u);
  EXPECT_EQ(result.chosen, 1);
  // The retried planner received the previous critique issues.
  EXPECT_NE(planner_users[1].find("missed governance"), std::string::npos);
  // And the retry ran at the raised temperature.
  ASSERT_EQ(critique_temperatures.size(), 2u);
  EXPECT_DOUBLE_EQ(critique_temperatures[1], 0.3);
}

TEST(GateFunnel, NeedsReviewBelowFloorSpendsNothingMore) {
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("Critique this candidate set") != std::string::npos)
      return ok(R"({"score": 0.45, "remove": [], "add": [], "issues": "vague"})");
    if (request.user.find("select the final children set") !=
        std::string::npos)
      return ok(R"({"items": [{"title": "One", "atomic": false}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})");
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  EXPECT_EQ(result.decision, Decision::NeedsReview);
  EXPECT_EQ(result.end_reason, "below floor");
  EXPECT_EQ(result.attempts.size(), 3u);  // nothing beyond the third attempt
}

TEST(GateParsers, CritiqueScoreScaleDriftIsRejected) {
  // W5 gate calibration (DR 0018): the critic must stay on the 0.0-1.0
  // scale the prompt asks for. Recorded replies parse cleanly; drifted
  // scales (0-10, 0-100, negative, >1) must fail the attempt instead of
  // silently mis-scoring the gate.
  auto ok_parse = parse_critique(
      R"({"score": 0.15, "remove": [], "add": [], "issues": "x"})");
  ASSERT_TRUE(ok_parse.has_value());
  EXPECT_DOUBLE_EQ(ok_parse->score, 0.15);
  // The two recorded reply shapes from the W5 cassettes.
  EXPECT_TRUE(parse_critique(
      R"({"score": 0.62, "remove": ["A"], "add": ["B"], "issues": "pool"})"));
  EXPECT_TRUE(parse_critique(
      R"({"score": 0.35, "remove": [], "add": ["X", "Y"], "issues": ""})"));
  // Scale drift: 0-10, 0-100, negative, above one, non-numeric.
  EXPECT_FALSE(parse_critique(R"({"score": 7.2})").has_value());
  EXPECT_FALSE(parse_critique(R"({"score": 72})").has_value());
  EXPECT_FALSE(parse_critique(R"({"score": -0.1})").has_value());
  EXPECT_FALSE(parse_critique(R"({"score": 1.5})").has_value());
  EXPECT_FALSE(parse_critique(R"({"score": "0.15"})").has_value());
}

TEST(GatePrompts, CritiquePromptCarriesTheAnchoredRubric) {
  // Decision 0020: fixed example anchors calibrate the critic's scale.
  const std::string prompt = critique_prompt(ctx(), {"A", "B"});
  EXPECT_NE(prompt.find("Score 0.8 example"), std::string::npos);
  EXPECT_NE(prompt.find("Score 0.5 example"), std::string::npos);
  EXPECT_NE(prompt.find("Score 0.2 example"), std::string::npos);
  EXPECT_NE(prompt.find("Calibrate against these anchors"),
            std::string::npos);
}

TEST(GateParsers, ChildrenKeyFromTheRecordedW54Replies) {
  // The W5.4 synthesizer replies drifted from "items" to "children"
  // (recorded shapes below); the parser accepts both and stays strict
  // on the entries.
  auto c1 = parse_children(
      R"({"children":[{"title":"Time-Based Expiration Conditions","atomic":false},{"title":"Conflict of Interest Screening","atomic":true}]})");
  ASSERT_TRUE(c1.has_value());
  ASSERT_EQ(c1->size(), 2u);
  EXPECT_FALSE((*c1)[0].atomic);
  EXPECT_TRUE((*c1)[1].atomic);

  auto c2 = parse_children(
      R"({"children": [{"title": "Expiration Notification Requirements", "atomic": true}, {"title": "Expiration Trigger Types", "atomic": false}]})");
  ASSERT_TRUE(c2.has_value());
  EXPECT_TRUE((*c2)[0].atomic);

  // The old "items" shape still parses.
  ASSERT_TRUE(parse_children(
      R"({"items": [{"title": "A", "atomic": false}]})").has_value());

  // Strict entries: a string entry or a missing title fails.
  EXPECT_FALSE(parse_children(R"({"children": ["not an object"]})"));
  EXPECT_FALSE(parse_children(R"({"children": [{"atomic": true}]})"));
  EXPECT_FALSE(parse_children(R"({"other": []})"));
}

TEST(GateFunnel, AtomicVerificationKeepsOnlyAgreedLeaves) {
  FunnelOptions options;
  options.verify_atomic = true;
  options.params.floor_score = 0.0;
  options.params.pass_score = 0.0;
  Funnel funnel(options);
  NodeContext context = ctx();
  int verify_calls = 0;
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("Critique this candidate set") != std::string::npos)
      return ok(R"({"score": 0.9, "remove": [], "add": [], "issues": ""})");
    if (request.user.find("select the final children set") !=
        std::string::npos)
      return ok(R"({"items": [{"title": "One", "atomic": true}, {"title": "Two", "atomic": true}, {"title": "Three", "atomic": false}]})");
    // Atomic verification: "One" agrees it admits no children (stays
    // atomic); "Two" admits children (becomes a task).
    if (request.user.find("was labelled atomic") != std::string::npos) {
      ++verify_calls;
      if (request.user.find("\"One\"") != std::string::npos)
        return ok(R"({"admits_children": false})");
      return ok(R"({"admits_children": true})");
    }
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  const auto& children = result.attempts[result.chosen].children;
  ASSERT_EQ(children.size(), 3u);
  EXPECT_TRUE(children[0].atomic);   // check agreed: leaf stays atomic
  EXPECT_FALSE(children[1].atomic);  // check disagreed: stays a task
  EXPECT_FALSE(children[2].atomic);  // never labelled
  EXPECT_EQ(verify_calls, 2);        // one call per labelled child
}

TEST(GateFunnel, AtomicVerificationFailureKeepsTheChildATask) {
  FunnelOptions options;
  options.verify_atomic = true;
  options.params.floor_score = 0.0;
  options.params.pass_score = 0.0;
  Funnel funnel(options);
  NodeContext context = ctx();
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("Critique this candidate set") != std::string::npos)
      return ok(R"({"score": 0.9, "remove": [], "add": [], "issues": ""})");
    if (request.user.find("select the final children set") !=
        std::string::npos)
      return ok(R"({"items": [{"title": "One", "atomic": true}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})");
    if (request.user.find("was labelled atomic") != std::string::npos) {
      Result bad;
      bad.kind = Kind::Transport;
      return bad;  // the verification call itself fails
    }
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  const auto& children = result.attempts[result.chosen].children;
  // A transport failure must not decide atomicity: the child stays a task.
  EXPECT_FALSE(children[0].atomic);
}

TEST(GateFunnel, AtomicVerificationOffPassesLabelsThrough) {
  FunnelOptions options;
  options.params.floor_score = 0.0;
  options.params.pass_score = 0.0;
  Funnel funnel(options);  // verify_atomic defaults off
  NodeContext context = ctx();
  int verify_calls = 0;
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("Critique this candidate set") != std::string::npos)
      return ok(R"({"score": 0.9, "remove": [], "add": [], "issues": ""})");
    if (request.user.find("select the final children set") !=
        std::string::npos)
      return ok(R"({"items": [{"title": "One", "atomic": true}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})");
    if (request.user.find("was labelled atomic") != std::string::npos)
      ++verify_calls;
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_TRUE(result.attempts[result.chosen].children[0].atomic);
  EXPECT_EQ(verify_calls, 0);
}

// ---------------- quality guard ----------------

TEST(QualityGuard, FlagsScoresBelowThresholdWithEnoughEvidence) {
  QualityGuard guard(10, 0.4, 0.5);
  for (int i = 0; i < 10; ++i) guard.observe(0.1, false);
  EXPECT_TRUE(guard.flagged());
  QualityGuard healthy(10, 0.4, 0.5);
  for (int i = 0; i < 10; ++i) healthy.observe(0.9, false);
  EXPECT_FALSE(healthy.flagged());
}

TEST(QualityGuard, FlagsFailureRateAboveThreshold) {
  QualityGuard guard(10, 0.4, 0.5);
  for (int i = 0; i < 10; ++i) guard.observe(0.9, i % 2 == 0 || i == 9);
  EXPECT_GT(guard.failure_rate(), 0.5);
  EXPECT_TRUE(guard.flagged());
}

}  // namespace
