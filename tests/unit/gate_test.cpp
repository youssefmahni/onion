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

// ---------------- decide (decision 0024: deterministic checks) ----------------

Attempt passing_attempt(int number) {
  Attempt attempt;
  attempt.number = number;
  attempt.score = 0.0;  // the score is irrelevant to the gate
  attempt.checks_passed = true;
  attempt.valid_count = 8;
  attempt.children = children_of(
      {"One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight"});
  return attempt;
}

Attempt failing_attempt(int number, std::size_t valid_count = 0) {
  Attempt attempt;
  attempt.number = number;
  attempt.score = 0.0;
  attempt.checks_passed = false;
  attempt.valid_count = valid_count;
  return attempt;
}

TEST(GateDecide, CommitsTheFirstAttemptWhoseChecksPassWhateverItsScore) {
  Attempt low_score = passing_attempt(1);
  low_score.score = 0.01;  // a critic score of ~0 must not matter
  Outcome outcome = decide({low_score}, GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Commit);
  EXPECT_EQ(outcome.chosen, 0);
  EXPECT_EQ(outcome.end_reason, "children");
  EXPECT_FALSE(outcome.below_target);
  outcome = decide({failing_attempt(1), passing_attempt(2)}, GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Commit);
  EXPECT_EQ(outcome.chosen, 1);
}

TEST(GateDecide, RetriesOnlyWhenAChecksFailed) {
  Outcome outcome = decide({failing_attempt(1)}, GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Retry);
  outcome = decide({failing_attempt(1), failing_attempt(2)}, GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Retry);
  // A passing first attempt never retries.
  EXPECT_EQ(decide({passing_attempt(1)}, GateParams{}).decision,
            Decision::Commit);
}

TEST(GateDecide, LeafWhenNoAttemptHasThreeValidChildren) {
  const Outcome outcome =
      decide({failing_attempt(1, 2), failing_attempt(2, 0),
              failing_attempt(3, 1)},
             GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Leaf);
  EXPECT_EQ(outcome.end_reason, "cannot split");
}

TEST(GateDecide, TheFallbackFloorIsThreeByDefaultAndAProfileRangeMinimumOtherwise) {
  // Planning default (decision 0024): 2 valid children never commit, whatever
  // the campaign target is.
  EXPECT_EQ(decide({failing_attempt(1, 2), failing_attempt(2, 2),
                    failing_attempt(3, 2)},
                   GateParams{})
                .decision,
            Decision::Leaf);
  // A domain profile whose range minimum is 2 (decision 0029) commits the
  // two valid children instead of leafing a legitimate two-step procedure.
  GateParams profile_params;
  profile_params.children_min = 2;
  profile_params.fallback_min = 2;
  const Outcome committed =
      decide({failing_attempt(1, 2), failing_attempt(2, 1),
              failing_attempt(3, 0)},
             profile_params);
  EXPECT_EQ(committed.decision, Decision::Commit);
  EXPECT_EQ(committed.chosen, 0);
  // ... and a profile minimum of 4 refuses a 3-child set.
  profile_params.children_min = 4;
  profile_params.fallback_min = 4;
  EXPECT_EQ(decide({failing_attempt(1, 3), failing_attempt(2, 3),
                    failing_attempt(3, 3)},
                   profile_params)
                .decision,
            Decision::Leaf);
}

TEST(GateDecide, ExhaustedAttemptsCommitTheMostValidChildrenBelowTarget) {
  // 4, 7, 5 valid children and no passing attempt: the 7-child attempt
  // commits its valid children, flagged below the target of 8.
  Outcome outcome =
      decide({failing_attempt(1, 4), failing_attempt(2, 7),
              failing_attempt(3, 5)},
             GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Commit);
  EXPECT_EQ(outcome.chosen, 1);
  EXPECT_TRUE(outcome.below_target);
  // Ties go to the later attempt.
  outcome = decide({failing_attempt(1, 6), failing_attempt(2, 6),
                    failing_attempt(3, 3)},
                   GateParams{});
  EXPECT_EQ(outcome.chosen, 1);
  // Nine valid children (one other title was bad) is not below target.
  outcome = decide({failing_attempt(1, 9), failing_attempt(2, 2),
                    failing_attempt(3, 2)},
                   GateParams{});
  EXPECT_EQ(outcome.decision, Decision::Commit);
  EXPECT_FALSE(outcome.below_target);
}

TEST(GateDecide, RetryInjectsHigherTemperatureViaFunnel) {
  // The funnel drives temperature 0 → 0.3 → 0.5 across attempts; the first
  // attempt whose checks pass commits, so a passing first attempt runs at
  // temperature 0 only.
  FunnelOptions options;
  options.params = GateParams{};
  options.params.children_min = 3;  // the small fakes return three children
  Funnel funnel(options);
  NodeContext context = ctx();
  std::vector<double> planner_temperatures;
  auto fake = [&](const Request& request) -> Result {
    Result result;
    result.kind = Kind::Ok;
    result.content = R"({"items": ["One", "Two", "Three"]})";
    result.finish_reason = "stop";
    if (request.user.find("Plan the knowledge coverage") != std::string::npos)
      planner_temperatures.push_back(request.temperature);
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
  ASSERT_EQ(planner_temperatures.size(), 1u);
  EXPECT_DOUBLE_EQ(planner_temperatures[0], 0.0);
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
  const auto verdict = parse_atomic_verdict(R"({"admits_none": ["A"]})");
  ASSERT_TRUE(verdict.has_value());
  EXPECT_EQ(verdict->size(), 1u);
  EXPECT_EQ((*verdict)[0], "A");
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

// F-GATE-2: a provider failure (outage, rate limit, rejected credentials,
// empty balance) says nothing about the node. It must never end as a leaf:
// a leaf is a permanent tombstone for the whole subtree.
TEST(GateFunnel, ProviderFailureNeverBecomesALeaf) {
  const std::pair<Kind, std::string> kinds[] = {
      {Kind::Transport, "transient"},   {Kind::Timeout, "transient"},
      {Kind::ServerError, "transient"}, {Kind::RateLimited, "transient"},
      {Kind::AuthError, "auth"},        {Kind::BalanceError, "balance"},
      {Kind::ClientError, "rejected"}};
  for (const auto& [kind, failure_class] : kinds) {
    FunnelOptions options;
    Funnel funnel(options);
    NodeContext context = ctx();
    int calls = 0;
    auto fake = [&](const Request&) -> Result {
      calls++;
      Result failure;
      failure.kind = kind;
      return failure;
    };
    const FunnelResult result = funnel.run(context, fake);
    EXPECT_NE(result.decision, Decision::Leaf)
        << "kind " << static_cast<int>(kind);
    EXPECT_EQ(result.decision, Decision::Defer)
        << "kind " << static_cast<int>(kind);
    EXPECT_EQ(result.end_reason, "provider unavailable");
    EXPECT_EQ(result.provider_failure, failure_class);
    EXPECT_TRUE(result.attempts.empty());  // nothing was learned or billed
    EXPECT_LE(calls, 4) << "a dead provider must not be hammered";
  }
}

// F-FUN-1: the ATTEMPT row's tokens and cost cover every call of the
// attempt — planner, workers, critique, synthesizer AND the atomic
// verification — and nothing from the pre-check.
TEST(GateFunnel, AttemptTokensCoverEveryCallOfTheAttempt) {
  FunnelOptions options;
  options.params.children_min = 3;  // the fake returns three children
  onion::llm::PriceSchedule prices;
  prices.peak.input = "1.0";
  prices.peak.cached_input = "0.0";
  prices.peak.output = "2.0";
  prices.offpeak = prices.peak;
  options.prices = prices;
  Funnel funnel(options);
  NodeContext context = ctx();
  int attempt_calls = 0;
  auto fake = [&](const Request& request) -> Result {
    Result reply = ok(R"({"items": ["Ops", "Gov", "Risk"]})");
    reply.cost_usd = "0.001";
    if (request.user.find("Can this node be split") != std::string::npos) {
      reply = ok(R"({"splits": true})");
      reply.in_miss = 1000;  // must not leak into the attempt
      reply.out = 1000;
      reply.cost_usd = "9";
      return reply;
    }
    if (request.user.find("Critique this candidate set") != std::string::npos)
      reply.content = R"({"score": 0.9, "remove": [], "add": [], "issues": ""})";
    if (request.user.find("select the final children set") !=
        std::string::npos)
      reply.content =
          R"({"items": [{"title": "One", "atomic": true}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})";
    if (request.user.find("admits_none") != std::string::npos)
      reply.content = R"({"admits_none": ["One"]})";
    attempt_calls++;
    reply.in_miss = 3;
    reply.out = 5;
    return reply;
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  ASSERT_EQ(result.attempts.size(), 1u);
  EXPECT_GE(attempt_calls, 5);  // planner, workers, synth, verify (critic off)
  const std::uint64_t calls = static_cast<std::uint64_t>(attempt_calls);
  EXPECT_EQ(result.attempts[0].tokens_in, 3 * calls);
  EXPECT_EQ(result.attempts[0].tokens_out, 5 * calls);
  // Priced at $1/M input and $2/M output: 3 in + 5 out per call = $13e-6.
  EXPECT_NEAR(std::stod(result.attempts[0].cost_usd), 13e-6 * attempt_calls,
              1e-12);
}

// One transient blip on the pre-check is retried by the funnel itself and
// is not a deferral; a blip mid-attempt defers the node (never leafs it).
TEST(GateFunnel, OneTransientBlipOnThePreCheckDoesNotDefer) {
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  int precheck_calls = 0;
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos) {
      if (++precheck_calls == 1) {
        Result failure;
        failure.kind = Kind::Transport;
        return failure;
      }
      return ok(R"({"splits": false})");
    }
    return ok(R"({"items": ["A", "B", "C"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  EXPECT_EQ(result.decision, Decision::Leaf);  // the model said "no split"
  EXPECT_TRUE(result.precheck_leaf);
  EXPECT_EQ(precheck_calls, 2);
}

TEST(GateFunnel, TransientFailureMidAttemptDefersAndKeepsEarlierAttempts) {
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  int synthesizer_calls = 0;
  auto fake = [&](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("Critique this candidate set") != std::string::npos)
      return ok(R"({"score": 0.1, "remove": [], "add": [], "issues": "weak"})");
    if (request.user.find("select the final children set") !=
        std::string::npos) {
      ++synthesizer_calls;
      if (synthesizer_calls == 2) {  // attempt 2: the provider drops
        Result failure;
        failure.kind = Kind::ServerError;
        return failure;
      }
      return ok(
          R"({"items": [{"title": "One", "atomic": false}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})");
    }
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  EXPECT_EQ(result.decision, Decision::Defer);
  EXPECT_EQ(result.provider_failure, "transient");
  ASSERT_EQ(result.attempts.size(), 1u);  // attempt 1 completed and is kept
  EXPECT_EQ(result.attempts[0].number, 1);
}

// ---- decision 0024: the funnel with the deterministic gate ----

// Distinct, filler-free titles ("A" would be dropped as an article by
// canonical() and collapse every title into one).
const char* kTitles[] = {
    "Invoice Matching",   "Payment Capture",   "Tax Calculation",
    "Order Routing",      "Stock Reservation", "Fraud Screening",
    "Address Validation", "Refund Handling",   "Carrier Selection",
    "Return Authorization", "Warehouse Picking", "Delivery Scheduling"};

std::string children_json(int count, bool atomic = false,
                          const std::string& prefix = "") {
  std::string json = R"({"items": [)";
  for (int i = 0; i < count; ++i) {
    if (i) json += ", ";
    json += R"({"title": ")" + prefix + kTitles[i] + R"(", "atomic": )" +
            (atomic ? "true" : "false") + "}";
  }
  return json + "]}";
}

std::string eight_children_json() { return children_json(8); }

// The standard fake: pre-check yes, planner/workers/trim return items,
// the synthesizer returns what `synth(call_index)` says.
SyncAsk gate_fake(std::function<Result(int, const Request&)> synth,
                  int* critique_calls = nullptr, int* trim_calls = nullptr) {
  auto synth_calls = std::make_shared<int>(0);
  return [=](const Request& request) -> Result {
    if (request.user.find("Can this node be split") != std::string::npos)
      return ok(R"({"splits": true})");
    if (request.user.find("Critique this candidate set") != std::string::npos) {
      if (critique_calls) ++*critique_calls;
      return ok(R"({"score": 0.05, "remove": [], "add": [], "issues": "meh"})");
    }
    if (request.user.find("From these candidates keep only") !=
        std::string::npos) {
      if (trim_calls) ++*trim_calls;
      return ok(R"({"items": ["T1", "T2", "T3"]})");
    }
    if (request.user.find("select the final children set") !=
        std::string::npos)
      return synth((*synth_calls)++, request);
    return ok(R"({"items": ["Alpha", "Beta", "Gamma", "Delta"]})");
  };
}

TEST(GateFunnel, CommitsOnPassingChecksWithoutAnyCritique) {
  // The critic is off by default: no critique call is made and the
  // attempt commits on the deterministic checks alone.
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  int critique_calls = 0;
  const FunnelResult result = funnel.run(
      context, gate_fake([](int, const Request&) {
        return ok(eight_children_json());
      }, &critique_calls));
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(critique_calls, 0);
  ASSERT_EQ(result.attempts.size(), 1u);
  EXPECT_EQ(result.attempts[0].children.size(), 8u);
  EXPECT_FALSE(result.below_target);
}

TEST(GateFunnel, CriticOnRecordsTheScoreButNeverGatesOrRetries) {
  FunnelOptions options;
  options.critic = true;
  Funnel funnel(options);
  NodeContext context = ctx();
  int critique_calls = 0;
  const FunnelResult result = funnel.run(
      context, gate_fake([](int, const Request&) {
        return ok(eight_children_json());
      }, &critique_calls));
  // A critic score of 0.05 (far below the old 0.5 floor) commits anyway.
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(critique_calls, 1);
  ASSERT_EQ(result.attempts.size(), 1u);
  EXPECT_DOUBLE_EQ(result.attempts[0].score, 0.05);
}

TEST(GateFunnel, RetriesOnlyOnCheckFailureAndInjectsTheReasons) {
  // Attempt 1 repeats the node's own title (a check failure); attempt 2
  // is clean. The retry runs hotter and BOTH the planner and the
  // synthesizer of attempt 2 are told what was wrong.
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  std::vector<std::string> synthesizer_prompts;
  std::vector<std::string> planner_prompts;
  std::vector<double> synthesizer_temperatures;
  auto base = gate_fake([&](int index, const Request& request) {
    synthesizer_prompts.push_back(request.user);
    synthesizer_temperatures.push_back(request.temperature);
    if (index == 0) {
      std::string json = eight_children_json();
      const std::size_t at = json.find("Invoice Matching");
      json.replace(at, std::string("Invoice Matching").size(),
                   "Order Capture");
      return ok(json);
    }
    return ok(eight_children_json());
  });
  const SyncAsk fake = [&](const Request& request) -> Result {
    if (request.user.find("Plan the knowledge coverage") != std::string::npos)
      planner_prompts.push_back(request.user);
    return base(request);
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  ASSERT_EQ(result.attempts.size(), 2u);
  EXPECT_EQ(result.chosen, 1);
  ASSERT_EQ(synthesizer_temperatures.size(), 2u);
  EXPECT_DOUBLE_EQ(synthesizer_temperatures[0], 0.0);
  EXPECT_DOUBLE_EQ(synthesizer_temperatures[1], 0.3);
  const std::string reason = "repeated title: Order Capture";
  EXPECT_EQ(synthesizer_prompts[0].find("Problems with your previous"),
            std::string::npos);
  EXPECT_NE(synthesizer_prompts[1].find(reason), std::string::npos);
  ASSERT_EQ(planner_prompts.size(), 2u);
  EXPECT_NE(planner_prompts[1].find(reason), std::string::npos);
}

TEST(GateFunnel, BelowTargetFallbackCommitsTheValidChildrenOnly) {
  // Five valid children on every attempt (the target is 8): after three
  // attempts the best commits its five, flagged below target.
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  const FunnelResult result = funnel.run(
      context, gate_fake([](int, const Request&) {
        return ok(R"({"items": [{"title": "Alpha One", "atomic": false}, {"title": "Beta Two", "atomic": false}, {"title": "Gamma Three", "atomic": false}, {"title": "Delta Four", "atomic": false}, {"title": "Epsilon Five", "atomic": false}]})");
      }));
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(result.attempts.size(), 3u);
  EXPECT_TRUE(result.below_target);
  EXPECT_EQ(result.attempts[result.chosen].children.size(), 5u);
}

TEST(GateFunnel, AnInvalidTitleIsDroppedNotCommitted) {
  // Nine good titles and one question: the checks fail every attempt (a
  // style violation), the best attempt commits its NINE valid children.
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  const FunnelResult result = funnel.run(
      context, gate_fake([](int, const Request&) {
        std::string json = R"({"items": [)";
        for (int i = 0; i < 9; ++i)
          json += R"({"title": ")" + std::string(kTitles[i]) +
                  R"(", "atomic": false}, )";
        json += R"({"title": "Is This A Question?", "atomic": false}]})";
        return ok(json);
      }));
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(result.attempts[result.chosen].children.size(), 9u);
  EXPECT_FALSE(result.below_target);
  for (const Child& child : result.attempts[result.chosen].children)
    EXPECT_EQ(child.title.find('?'), std::string::npos);
}

TEST(GateFunnel, FewerThanThreeValidChildrenIsALeaf) {
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  const FunnelResult result = funnel.run(
      context, gate_fake([](int, const Request&) {
        return ok(R"({"items": [{"title": "Only One", "atomic": false}, {"title": "Only Two", "atomic": false}]})");
      }));
  EXPECT_EQ(result.decision, Decision::Leaf);
  EXPECT_EQ(result.end_reason, "cannot split");
  EXPECT_EQ(result.attempts.size(), 3u);  // every attempt completed
}

TEST(GateFunnel, TrimIsSkippedWhenThePoolIsAtOrBelowTheTarget) {
  // Workers return 4 candidates per aspect (well under the 150 target):
  // no trim call at all. With a 400-candidate pool exactly ONE trim runs.
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  int trim_calls = 0;
  FunnelResult result = funnel.run(
      context, gate_fake([](int, const Request&) {
        return ok(eight_children_json());
      }, nullptr, &trim_calls));
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(trim_calls, 0);

  trim_calls = 0;
  const SyncAsk big = [&](const Request& request) -> Result {
    if (request.user.find("Aspect:") != std::string::npos) {
      // 100 distinct candidates per aspect, distinct across aspects.
      const std::size_t at = request.user.find("Aspect: ") + 8;
      const std::string aspect =
          request.user.substr(at, request.user.find('\n', at) - at);
      std::string json = R"({"items": [)";
      for (int i = 0; i < 100; ++i) {
        if (i) json += ", ";
        json += "\"" + aspect + " Candidate " + std::to_string(i * 7919) +
                "\"";
      }
      return ok(json + "]}");
    }
    if (request.user.find("Plan the knowledge coverage") != std::string::npos)
      return ok(R"({"items": ["Aspect Aaaa", "Aspect Bbbb", "Aspect Cccc"]})");
    return gate_fake([](int, const Request&) {
      return ok(eight_children_json());
    }, nullptr, &trim_calls)(request);
  };
  Funnel funnel_big(options);
  result = funnel_big.run(context, big);
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(trim_calls, 1);
}

TEST(GateFunnel, VerificationRunsOnlyOnTheChosenAttempt) {
  // Attempts 1 and 2 fail the style check but carry atomic labels; attempt
  // 3 passes. Exactly ONE verification call is made, listing attempt 3's
  // labelled titles only.
  FunnelOptions options;
  Funnel funnel(options);
  NodeContext context = ctx();
  std::vector<std::string> verification_prompts;
  // Attempts 1-2: the first title is a question (style failure) and
  // every title carries an atomic label; attempt 3 is clean.
  auto labelled_json = [](const std::string& prefix, bool bad) {
    std::string json = R"({"items": [)";
    for (int i = 0; i < 8; ++i) {
      if (i) json += ", ";
      json += R"({"title": ")" + prefix + kTitles[i] + (bad && i == 0 ? "?" : "") +
              R"(", "atomic": true})";
    }
    return json + "]}";
  };
  auto base = gate_fake([&](int index, const Request&) {
    return ok(labelled_json(index < 2 ? "Faulty " : "Sound ", index < 2));
  });
  const SyncAsk fake = [&](const Request& request) -> Result {
    if (request.user.find("were labelled atomic") != std::string::npos) {
      verification_prompts.push_back(request.user);
      return ok(R"({"admits_none": []})");
    }
    return base(request);
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(result.chosen, 2);
  ASSERT_EQ(verification_prompts.size(), 1u);
  EXPECT_NE(verification_prompts[0].find("Sound Invoice Matching"),
            std::string::npos);
  EXPECT_EQ(verification_prompts[0].find("Faulty"), std::string::npos);
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

TEST(GateFunnel, BatchedAtomicVerificationKeepsOnlyNamedLeaves) {
  FunnelOptions options;
  options.verify_atomic = true;
  options.params.children_min = 3;  // these fakes return three children
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
    // Decision 0021: ONE batched call lists the labelled children; the
    // reply names which admit no decomposition.  "One" is named, "Two"
    // is not — so "Two" stays a task.
    if (request.user.find("were labelled atomic") != std::string::npos) {
      ++verify_calls;
      EXPECT_NE(request.user.find("One"), std::string::npos);
      EXPECT_NE(request.user.find("Two"), std::string::npos);
      return ok(R"({"admits_none": ["One"]})");
    }
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  const auto& children = result.attempts[result.chosen].children;
  ASSERT_EQ(children.size(), 3u);
  EXPECT_TRUE(children[0].atomic);   // named by the check: leaf
  EXPECT_FALSE(children[1].atomic);  // not named: stays a task
  EXPECT_FALSE(children[2].atomic);  // never labelled
  EXPECT_EQ(verify_calls, 1);        // ONE call for the whole attempt
}

TEST(GateFunnel, BatchedAtomicVerificationFailureKeepsEveryChildATask) {
  FunnelOptions options;
  options.verify_atomic = true;
  options.params.children_min = 3;  // these fakes return three children
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
    if (request.user.find("were labelled atomic") != std::string::npos) {
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

TEST(GateFunnel, BatchedVerificationRunsByDefault) {
  FunnelOptions options;  // verify_atomic defaults on (decision 0021)
  options.params.children_min = 3;  // these fakes return three children
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
      return ok(R"({"items": [{"title": "One", "atomic": true}, {"title": "Two", "atomic": false}, {"title": "Three", "atomic": false}]})");
    if (request.user.find("were labelled atomic") != std::string::npos) {
      ++verify_calls;
      return ok(R"({"admits_none": []})");
    }
    return ok(R"({"items": ["A", "B", "C", "D"]})");
  };
  const FunnelResult result = funnel.run(context, fake);
  ASSERT_EQ(result.decision, Decision::Commit);
  EXPECT_EQ(verify_calls, 1);
  // The check named nothing: the label does not survive.
  EXPECT_FALSE(result.attempts[result.chosen].children[0].atomic);
}

TEST(GateParsers, BatchedAtomicVerdictIsStrict) {
  auto verdict = parse_atomic_verdict(R"({"admits_none": ["One", "Two"]})");
  ASSERT_TRUE(verdict.has_value());
  ASSERT_EQ(verdict->size(), 2u);
  EXPECT_EQ((*verdict)[0], "One");
  EXPECT_TRUE(parse_atomic_verdict(R"({"admits_none": []})").has_value());
  EXPECT_FALSE(parse_atomic_verdict(R"({"admits_none": ["One", 2]})"));
  EXPECT_FALSE(parse_atomic_verdict(R"({"other": []})"));
  EXPECT_FALSE(parse_atomic_verdict(R"({"admits_none": "One"})"));
}

// ---------------- quality guard ----------------// ---------------- quality guard ----------------

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
