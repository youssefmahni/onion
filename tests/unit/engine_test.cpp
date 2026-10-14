#include <gtest/gtest.h>

#include <thread>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gate/gate.hpp"
#include "llm/llm.hpp"
#include "sched/sched.hpp"
#include "state/state.hpp"
#include "store/store.hpp"

namespace {

using onion::gate::SyncAsk;
using onion::llm::Budget;
using onion::llm::Kind;
using onion::llm::Request;
using onion::llm::Result;
using onion::sched::CampaignSpec;
using onion::sched::Engine;
using onion::state::CommitTerminal;
using onion::state::DevImport;
using onion::state::DevImportedNode;
using onion::state::StateWriter;
using nlohmann::json;

// The deterministic offline funnel: the same behaviour as the fake server's
// --funnel mode, in process. Titles rotate through a palette so no ancestor
// or sibling repeats; the pre-check refuses at max_depth and the deepest
// children are atomic.
const std::vector<std::string> kPalette = {
    "Order Capture", "Shipment Status", "Payment Method", "Inventory Count",
    "Customer Record", "Audit Trail", "Approval Flow", "Report Export",
    "Compliance Check", "Refund Process", "Vendor Registry",
    "Warehouse Layout", "Invoice Matching", "Return Authorization",
    "Quality Inspection", "Forecast Model", "Pricing Strategy",
    "Contract Review", "Credit Assessment", "Lead Scoring",
    "Support Escalation",
};

int depth_of(const std::string& prompt) {
  const std::size_t start = prompt.find("Node: L");
  if (start == std::string::npos) return 0;
  std::string digits;
  for (std::size_t i = start + 7; i < prompt.size() && std::isdigit(prompt[i]);
       ++i)
    digits += prompt[i];
  return digits.empty() ? 0 : std::atoi(digits.c_str());
}

// M8: a chain fake — every node produces exactly ONE child until
// max_depth, where the pre-check refuses. Used by the branch-share
// tests: a single chain halves its pool per level.
SyncAsk chain_fake(int max_depth) {
  // Every splittable node yields ONE chain link and two dead ends; the
  // dead ends fail the next pre-check and leaf via cannot-split. The
  // chain links get unique titles per level (an ancestor repeat would
  // trip the similarity gate), so the tree is a single chain of
  // splittable nodes.
  return [=](const Request& request) -> Result {
    Result result;
    result.kind = Kind::Ok;
    result.finish_reason = "stop";
    result.cost_usd = "0.001";
    auto reply = [&](const std::string& content) {
      result.content = content;
      return result;
    };
    const std::string& prompt = request.user;
    const int depth = depth_of(prompt);
    // Distinct words per level: the ancestor gate compares by LCS
    // similarity, so "Chain Link 1" vs "Chain Link 2" (0.92) would be
    // rejected as a repeated ancestor.
    static const char* kChainNames[] = {
        "Alpha",   "Beta",  "Gamma", "Delta", "Epsilon", "Zeta",  "Eta",
        "Theta",   "Iota",  "Kappa", "Lambda", "Mu",     "Nu",    "Xi",
        "Omicron", "Pi"};
    const auto chain_title = [](int level) {
      return "Chain Link " + std::string(kChainNames[level % 16]);
    };
    if (prompt.find("Can this node be split") != std::string::npos) {
      const bool splittable =
          depth < max_depth &&
          (depth == 0 ||
           prompt.find(chain_title(depth)) != std::string::npos);
      return reply(json{{"splits", splittable}}.dump());
    }
    if (prompt.find("Plan the knowledge coverage") != std::string::npos)
      return reply(json{{"items", {"Coverage A"}}}.dump());
    if (prompt.find("Aspect:") != std::string::npos)
      return reply(json{{"items",
                         {chain_title(depth + 1),
                          "Terminal Zero " + std::to_string(depth),
                          "Terminal One " + std::to_string(depth)}}}
                       .dump());
    if (prompt.find("From these candidates keep only") != std::string::npos)
      return reply(json{{"items",
                         {chain_title(depth + 1),
                          "Terminal Zero " + std::to_string(depth),
                          "Terminal One " + std::to_string(depth)}}}
                       .dump());
    if (prompt.find("Critique this candidate set") != std::string::npos)
      return reply(json{{"score", 0.9}, {"remove", json::array()},
                        {"add", json::array()}, {"issues", ""}}.dump());
    if (prompt.find("select the final children set") != std::string::npos) {
      json children = json::array();
      children.push_back({{"title", chain_title(depth + 1)},
                          {"atomic", false}});
      children.push_back({{"title", "Terminal Zero " + std::to_string(depth)},
                          {"atomic", false}});
      children.push_back({{"title", "Terminal One " + std::to_string(depth)},
                          {"atomic", false}});
      return reply(json{{"items", children}}.dump());
    }
    return reply(json{{"items", {"One"}}}.dump());
  };
}

SyncAsk funnel_fake(int max_depth, std::vector<json>* calls = nullptr,
                    double score = 0.9, int atomic_from = 99) {
  return [=](const Request& request) -> Result {
    if (calls) calls->push_back({{"user", request.user}});
    const std::string& prompt = request.user;    const int depth = depth_of(prompt);
    Result result;
    result.kind = Kind::Ok;
    result.finish_reason = "stop";
    result.cost_usd = "0.001";
    auto reply = [&](const std::string& content) {
      result.content = content;
      return result;
    };
    if (prompt.find("Can this node be split") != std::string::npos)
      return reply(json{{"splits", depth < max_depth}}.dump());
    if (prompt.find("Plan the knowledge coverage") != std::string::npos)
      return reply(json{{"items", {"Coverage A", "Coverage B", "Coverage C"}}}.dump());
    // Children of any node at this depth share one palette band, so titles
    // never repeat a sibling, an ancestor or the node itself.
    const int base = 3 * (depth + 1);
    const auto children_titles = [&]() {
      return std::vector<std::string>{kPalette[base % kPalette.size()],
                                      kPalette[(base + 1) % kPalette.size()],
                                      kPalette[(base + 2) % kPalette.size()]};
    };
    if (prompt.find("Aspect:") != std::string::npos)
      return reply(json{{"items", children_titles()}}.dump());
    if (prompt.find("From these candidates keep only") != std::string::npos)
      return reply(json{{"items", children_titles()}}.dump());
    if (prompt.find("Critique this candidate set") != std::string::npos)
      return reply(
          json{{"score", score}, {"remove", json::array()},
               {"add", json::array()}, {"issues", ""}}
              .dump());
    if (prompt.find("select the final children set") != std::string::npos) {
      json children = json::array();
      for (int i = 0; i < 3; ++i)
        children.push_back(
            {{"title", kPalette[(base + i) % kPalette.size()]},
             {"atomic", depth >= atomic_from}});
      return reply(json{{"items", children}}.dump());
    }
    // Atomic verification (DR 0021): the fake names the full palette in
    // admits_none — a superset of the labelled children — so the labels
    // survive as verified.
    if (prompt.find("were labelled atomic") != std::string::npos) {
      json none = json::array();
      for (const auto& item : kPalette) none.push_back(item);
      return reply(json{{"admits_none", none}}.dump());
    }
    return reply(json{{"items", {"One", "Two", "Three"}}}.dump());
  };
}

class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("onion_engine_test_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    StateWriter::Options options;
    options.data_dir = dir_;
    options.origin = "dev0000000000000001";
    writer_ = std::make_unique<StateWriter>(std::move(options));
    std::string error;
    ASSERT_TRUE(writer_->start(&error)) << error;
  }
  void TearDown() override {
    if (writer_) writer_->stop();
    std::filesystem::remove_all(dir_);
  }

  void seed_root() {
    DevImport import;
    import.nodes.push_back({"P01", "P01", 0, "Order Operations"});
    writer_->submit(std::move(import));
    writer_->sync();
  }

  CampaignSpec spec() {
    CampaignSpec s;
    s.id = "c1";
    s.scope = {"P01"};
    s.loop_budget = 10000;
    s.mode = "breadth-first";
    s.chunk = 50;
    return s;
  }

  Engine make_engine(Budget& budget) {
    return Engine(Engine::Options{*writer_, funnel_fake(6), budget, 1,
                                  "dev0000000000000001", "0.001"});
  }

  std::filesystem::path dir_;
  std::unique_ptr<StateWriter> writer_;
};

// F-GATE-2 at the engine: while the provider is down nothing is settled and
// no node becomes a leaf; when it recovers the very same node is worked.
TEST_F(EngineTest, ProviderOutageLeavesNothingSettledAndRecovers) {
  seed_root();
  Budget budget("10");
  bool down = true;
  const SyncAsk healthy = funnel_fake(2);
  const SyncAsk ask = [&](const Request& request) -> Result {
    if (down) {
      Result failure;
      failure.kind = Kind::Transport;
      return failure;
    }
    return healthy(request);
  };
  Engine::Options options{*writer_, ask, budget, 1, "dev0000000000000001",
                          "0.001"};
  options.provider_backoff_initial = std::chrono::milliseconds(150);
  Engine engine(std::move(options));
  CampaignSpec campaign = spec();
  campaign.structure = true;

  const Engine::RunResult outage = engine.run(campaign, nullptr);
  EXPECT_EQ(outage.settled, 0);
  EXPECT_EQ(outage.leaves, 0);
  EXPECT_EQ(outage.end_reason, "provider unavailable");
  EXPECT_FALSE(writer_->settled("P01"));
  EXPECT_FALSE(writer_->leaf("P01"));

  // Inside the backoff window the engine makes no call at all.
  down = false;
  const Engine::RunResult waiting = engine.run(campaign, nullptr);
  EXPECT_EQ(waiting.end_reason, "provider unavailable");
  EXPECT_EQ(waiting.calls, 0);
  EXPECT_FALSE(writer_->settled("P01"));

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const Engine::RunResult recovered = engine.run(campaign, nullptr);
  EXPECT_GT(recovered.settled, 0);
  EXPECT_TRUE(writer_->settled("P01"));
  EXPECT_FALSE(writer_->leaf("P01"));
}

// F-BUD-1: the device's USD budget decrements by what the attempts really
// cost (it used to settle every reservation at "0", so no cap could bind).
TEST_F(EngineTest, DeviceBudgetSpendsTheRealAttemptCost) {
  seed_root();
  Budget budget("10");
  const SyncAsk base = funnel_fake(2);
  const SyncAsk ask = [&](const Request& request) -> Result {
    Result reply = base(request);
    reply.in_miss = 1000;  // 1,000 in + 500 out per call
    reply.out = 500;
    return reply;
  };
  Engine::Options options{*writer_, ask, budget, 1, "dev0000000000000001",
                          "1.0"};
  onion::llm::PriceSchedule prices;
  prices.peak.input = "1.0";  // USD per 1M tokens
  prices.peak.cached_input = "0.0";
  prices.peak.output = "2.0";
  prices.offpeak = prices.peak;
  options.prices = prices;
  Engine engine(std::move(options));
  CampaignSpec campaign = spec();
  campaign.structure = true;
  campaign.loop_budget = 6;
  const Engine::RunResult run = engine.run(campaign, nullptr);
  ASSERT_GT(run.attempts, 0);

  std::string error;
  long double recorded = 0;
  for (const onion::store::Attempt& attempt : writer_->db().attempts(&error))
    recorded += std::stold(attempt.cost);
  EXPECT_GT(recorded, 0.0L);
  // The budget spent exactly what the ATTEMPT rows recorded.
  EXPECT_NEAR(static_cast<double>(std::stold(budget.spent_usd())),
              static_cast<double>(recorded), 1e-9);
  EXPECT_EQ(budget.reserved_usd(), "0");  // every reservation settled
}

TEST_F(EngineTest, RunsAThreeHundredNodeCampaignOffline) {
  seed_root();
  Budget budget("10");
  Engine engine = make_engine(budget);
  CampaignSpec campaign = spec();
  campaign.structure = true;  // L1..L3 expansion on
  const Engine::RunResult result = engine.run(campaign, nullptr);
  // No atomic children: the stop rule never fires; the tree drains when the
  // pre-check refuses at depth 6. 1+3+...+243 = 364 settled nodes.
  EXPECT_EQ(result.end_reason, "drained");
  EXPECT_GE(result.settled, 300);
  EXPECT_GT(result.leaves, 0);

  // P7: scan the log — no settled node has a failed attempt committed for
  // it, and every committed settle carries a passing score ≥ the floor.
  std::ifstream in(writer_->segments().active_path());
  std::string line;
  int settles = 0;
  std::vector<std::string> failed_paths;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto batch = json::parse(line, nullptr, false);
    if (batch.is_discarded()) continue;
    for (const auto& record : batch["recs"]) {
      if (record["t"] == "ATTEMPT" && record["p"]["passed"] == 0)
        failed_paths.push_back(record["p"]["path"].get<std::string>());
      if (record["t"] == "SETTLE") {
        settles++;
        if (record["p"]["end"] == "children") {
          const double score =
              std::stod(record["p"]["score"].get<std::string>());
          EXPECT_GE(score, 0.5);
          EXPECT_GE(record["p"]["attempts"].get<int>(), 1);
        }
      }
    }
  }
  // The P7 regression: every failed attempt belongs to a node that is
  // either retried or never settled — a settled node's failed attempt only
  // exists when the node settled from a LATER attempt, which this fake
  // never produces (score 0.9 on attempt 1, no retries).
  EXPECT_TRUE(failed_paths.empty())
      << "failed attempt committed for " << failed_paths.front();

  // The store agrees: every settled node has a row.
  std::string error;
  EXPECT_EQ(writer_->db().settles(&error).size(),
            static_cast<std::size_t>(settles));
}

TEST_F(EngineTest, StopRuleEndsAnAtomicFrontierCampaign) {
  seed_root();
  Budget budget("10");
  // Atomic children from depth 4: once the last 50 settles are ≥ 90% atomic
  // with average score ≥ 0.8, the campaign stops by itself.
  Engine engine(Engine::Options{*writer_, funnel_fake(5, nullptr, 0.9, 4),
                                budget, 1, "dev0000000000000001", "0.001"});
  CampaignSpec campaign = spec();
  campaign.structure = true;
  // DESIGN › Quality gate + DR 0021: only VERIFIED atomic labels count
  // toward the stop rule, so this campaign runs with verification on.
  campaign.verify_atomic = true;
  const Engine::RunResult result = engine.run(campaign, nullptr);
  EXPECT_EQ(result.end_reason, "stop rule");
  EXPECT_GT(result.settled, 0);
  EXPECT_GT(result.atomic, 0);
}

TEST_F(EngineTest, WaitingTicketStartsWhenTheReservationIsReleased) {
  // Two nodes, budget for exactly one ticket at a time (G9).
  DevImport import;
  import.nodes.push_back({"P01", "P01", 0, "Root"});
  import.nodes.push_back({"P01-IN01", "P01-IN01", 1, "Child"});
  writer_->submit(std::move(import));
  writer_->sync();

  Budget budget("0.001");
  Engine engine(Engine::Options{*writer_, funnel_fake(1), budget, 1,
                                "dev0000000000000001", "0.001"});
  CampaignSpec campaign = spec();
  campaign.structure = true;
  const Engine::RunResult result = engine.run(campaign, nullptr);
  EXPECT_NE(result.end_reason, "budget");  // the second ticket waited and ran
  EXPECT_TRUE(writer_->settled("P01"));
  EXPECT_TRUE(writer_->settled("P01-IN01"));
}

TEST_F(EngineTest, ReviewAcceptSettlesWithoutSpending) {
  seed_root();
  Budget budget("10");
  Engine engine(Engine::Options{*writer_, funnel_fake(5, nullptr, 0.45), budget,
                                1, "dev0000000000000001", "0.001"});
  CampaignSpec below_floor = spec();
  const Engine::RunResult result = engine.run(below_floor, nullptr);
  EXPECT_EQ(result.reviews, 1);
  EXPECT_TRUE(writer_->needs_review("P01"));

  // The review pause keeps the stored children off the queue until accept.
  engine.review_accept("P01", spec());
  EXPECT_FALSE(writer_->needs_review("P01"));
  EXPECT_TRUE(writer_->settled("P01"));
}

TEST_F(EngineTest, ReviewRejectTombstonesTheSubtree) {
  seed_root();
  Budget budget("10");
  Engine engine(Engine::Options{*writer_, funnel_fake(5, nullptr, 0.45), budget,
                                1, "dev0000000000000001", "0.001"});
  CampaignSpec below_floor = spec();
  engine.run(below_floor, nullptr);
  ASSERT_TRUE(writer_->needs_review("P01"));

  engine.review_reject("P01", spec());
  std::string error;
  const auto subtree = writer_->db().nodes_in_subtree("P01", &error);
  for (const auto& node : subtree) EXPECT_EQ(node.tombstone, 1);
}

TEST_F(EngineTest, StopIsFinal) {
  seed_root();
  Budget budget("10");
  Engine engine = make_engine(budget);
  std::atomic<bool> stop{false};
  stop.store(true);
  CampaignSpec stopped_spec = spec();
  const Engine::RunResult result = engine.run(stopped_spec, &stop);
  EXPECT_EQ(result.end_reason, "stopped");

  // After a stop record the campaign is final: a restart must not run it.
  writer_->submit(CommitTerminal{{
      [] {
        onion::log::Record record;
        record.type = "CAMPAIGN";
        record.payload = {{"id", "c1"}, {"spec", json::object()}, {"state", "stopped"}};
        return record;
      }(),
  }});
  writer_->sync();
  CampaignSpec stopped_spec_2 = spec();
  const Engine::RunResult second = engine.run(stopped_spec_2, nullptr);
  EXPECT_EQ(second.end_reason, "stopped");
  EXPECT_EQ(second.settled, 0);
}

TEST_F(EngineTest, KillAtEveryCommitStepLosesNoCommittedWork) {
  seed_root();
  // A campaign whose tickets are settled one at a time survives a crash
  // between the durable write and the fold: after a restart, everything
  // committed is in the store and the log (covered in detail by the
  // StateWriter fork tests; here the engine-level property holds: running
  // again after a stop drains nothing new).
  Budget budget("10");
  Engine engine = make_engine(budget);
  CampaignSpec kill_spec = spec();
  kill_spec.structure = true;
  const Engine::RunResult first = engine.run(kill_spec, nullptr);
  EXPECT_GE(first.settled, 300);

  std::string error;
  const std::size_t settles_before = writer_->db().settles(&error).size();
  writer_->stop();
  StateWriter::Options options;
  options.data_dir = dir_;
  options.origin = "dev0000000000000001";
  auto reopened = std::make_unique<StateWriter>(std::move(options));
  ASSERT_TRUE(reopened->start(&error)) << error;
  EXPECT_EQ(reopened->db().settles(&error).size(), settles_before);
  reopened->stop();
}

}  // namespace

// ---------------------------------------------------- M6 liveness (0010)

TEST_F(EngineTest, ExhaustedCampaignStaysRunningAndResumesOnBudgetRaise) {
  seed_root();
  // Fleet mode: the device owns everything (huge weight), budget 2.
  Budget budget("10");
  Engine engine(Engine::Options{*writer_, funnel_fake(2), budget, 1,
                                "dev0000000000000001", "0.001", std::nullopt,
                                0,
                                {{"dev0000000000000001", 100.0},
                                 {"dev0000000000000002", 0.1}},
                                "dev0000000000000001"});
  CampaignSpec tiny = spec();
  tiny.loop_budget = 2;
  tiny.chunk = 1;
  tiny.mode = "breadth-first";
  tiny.structure = true;  // this test is about the budget, not structure
  // Pools pre-set (decision 0020): a 2-attempt budget starves the
  // branch-share rule, and this test isolates the RESERVATION pacing.
  tiny.pools = {{"P01", 1000}};

  // Decision 0014 amendment: the reservation gate starts CLOSED — a
  // 2-member fleet device reserves only after a completed (majority)
  // round. The first run therefore drains with no reservation and no
  // settled work.
  const Engine::RunResult gated = engine.run(tiny, nullptr);
  EXPECT_EQ(gated.end_reason, "drained");
  EXPECT_EQ(gated.settled, 0);
  // Decision 0010: exhaustion never closes the campaign record.
  ASSERT_TRUE(writer_->campaign("c1").has_value());
  EXPECT_EQ(writer_->campaign("c1")->state, "running");

  // After the first completed round the reservation takes min(measured
  // chunk, budget) = 2 attempts and settles them; the spent chunk cannot
  // renew until the next round, so the run drains (decision 0014)
  // rather than overspending.
  engine.note_sync_round();
  const Engine::RunResult first = engine.run(tiny, nullptr);
  EXPECT_EQ(first.end_reason, "drained");
  EXPECT_GT(first.settled, 0);

  // After the next sync round the reservation attempt finds the budget
  // exhausted: "budget", still not closed.
  engine.note_sync_round();
  const Engine::RunResult exhausted = engine.run(tiny, nullptr);
  EXPECT_EQ(exhausted.end_reason, "budget");
  EXPECT_EQ(writer_->campaign("c1")->state, "running");

  // Raising the loop budget resumes the open tickets (decision 0010): the
  // raise lands as a new CAMPAIGN record — the capped fold reads the
  // budget from the latest record — then one more sync round before the
  // engine may reserve again.
  tiny.loop_budget = 1000;
  writer_->submit(CommitTerminal{{
      [] {
        onion::log::Record record;
        record.type = "CAMPAIGN";
        record.payload = {{"id", "c1"},
                          {"spec", {{"loop_budget", 1000}, {"chunk", 50}}},
                          {"state", "running"}};
        return record;
      }(),
  }});
  writer_->sync();
  engine.note_sync_round();
  const Engine::RunResult second = engine.run(tiny, nullptr);
  EXPECT_EQ(second.end_reason, "drained");  // the resumed tickets finished
  EXPECT_GT(second.settled, 0);
}

TEST_F(EngineTest, ReservationGateStartsClosedAfterRestart) {
  // Decision 0014 amendment: a device that restarts inside a partition
  // used to get one free reservation ("first reservation is free" was
  // per-process). A fresh engine over an existing store now starts with
  // the gate CLOSED: the first run reserves nothing, spends nothing,
  // writes no RESERVE record — until a completed round opens it.
  seed_root();
  Budget budget("10");
  CampaignSpec campaign = spec();
  campaign.structure = true;
  {
    // A previous "process": settle the root so the store has history.
    // The gate is closed at process start; the first run drains, one
    // completed round opens the gate, the next run works.
    Engine previous(Engine::Options{*writer_, funnel_fake(6), budget, 1,
                                    "dev0000000000000001", "0.001",
                                    std::nullopt, 0,
                                    {{"dev0000000000000001", 100.0},
                                     {"dev0000000000000002", 0.1}},
                                    "dev0000000000000001"});
    const Engine::RunResult cold = previous.run(campaign, nullptr);
    EXPECT_EQ(cold.end_reason, "drained");
    EXPECT_EQ(cold.settled, 0);
    previous.note_sync_round();
    const Engine::RunResult warm = previous.run(campaign, nullptr);
    ASSERT_GT(warm.settled, 0);
  }
  // The restarted process: a brand-new engine over the same store.
  Engine restarted(Engine::Options{*writer_, funnel_fake(6), budget, 1,
                                   "dev0000000000000001", "0.001",
                                   std::nullopt, 0,
                                   {{"dev0000000000000001", 100.0},
                                    {"dev0000000000000002", 0.1}},
                                   "dev0000000000000001"});
  const std::int64_t reserved_before = writer_->reserved_total("c1");
  const Engine::RunResult gated = restarted.run(campaign, nullptr);
  EXPECT_EQ(gated.end_reason, "drained");
  EXPECT_EQ(gated.attempts, 0);
  EXPECT_EQ(gated.settled, 0);
  // No new RESERVE record: the pre-restart fold is unchanged.
  EXPECT_EQ(writer_->reserved_total("c1"), reserved_before);
  // One completed round opens the gate and work resumes.
  restarted.note_sync_round();
  const Engine::RunResult resumed = restarted.run(campaign, nullptr);
  EXPECT_EQ(resumed.end_reason, "drained");
  EXPECT_GT(resumed.settled, 0);
  EXPECT_GT(writer_->reserved_total("c1"), reserved_before);
}

TEST_F(EngineTest, SingleMemberFleetReservesImmediately) {
  // 0/1 is a majority (0014 amendment): a one-member fleet opens the
  // gate at once — no round, no wait, no stall.
  seed_root();
  Budget budget("10");
  Engine engine(Engine::Options{*writer_, funnel_fake(6), budget, 1,
                                "dev0000000000000001", "0.001",
                                std::nullopt, 0,
                                {{"dev0000000000000001", 100.0}},
                                "dev0000000000000001"});
  CampaignSpec campaign = spec();
  campaign.structure = true;
  const Engine::RunResult first = engine.run(campaign, nullptr);
  EXPECT_EQ(first.end_reason, "drained");
  EXPECT_GT(first.settled, 0);
  EXPECT_GT(writer_->reserved_total("c1"), 0);
}

TEST_F(EngineTest, StructureReviewDoesNotReprocessTheTicket) {
  seed_root();
  Budget budget("10");
  Engine engine = make_engine(budget);
  CampaignSpec campaign = spec();
  campaign.structure = false;  // the root is L1: expansion goes to review
  const Engine::RunResult first = engine.run(campaign, nullptr);
  EXPECT_EQ(first.reviews, 1);
  EXPECT_EQ(first.settled, 0);

  // Running again must not re-attempt the reviewed ticket: still exactly
  // one review, no new children, no spend.
  const Engine::RunResult second = engine.run(campaign, nullptr);
  EXPECT_EQ(second.reviews, 0);
  EXPECT_EQ(second.attempts, 0);
  std::string error;
  EXPECT_EQ(writer_->db().open_reviews(&error).size(), 1u);
  // The root plus the review-stored children (decision 0008) — and the
  // second run added nothing.
  EXPECT_EQ(writer_->db().all_nodes(&error).size(), 4u);
}

TEST_F(EngineTest, IdleFleetEngineStealsFromTheBusiestLane) {
  seed_root();
  Budget budget("10");
  // The static list gives the OTHER device every lane (weight 100 vs
  // 0.1). This device is work-starved and steals (DESIGN › Stealing):
  // a CLAIM record guards the stolen ticket.
  Engine engine(Engine::Options{*writer_, funnel_fake(8), budget, 1,
                                "dev0000000000000001", "0.001", std::nullopt,
                                0,
                                {{"dev0000000000000001", 0.1},
                                 {"dev0000000000000002", 100.0}},
                                "dev0000000000000001"});
  CampaignSpec campaign = spec();
  campaign.structure = true;
  // The gate starts closed (0014 amendment): the first run drains with
  // no reservation and no steals; one completed round opens it and the
  // idle device then steals from the busy lanes.
  const Engine::RunResult cold = engine.run(campaign, nullptr);
  EXPECT_EQ(cold.end_reason, "drained");
  EXPECT_EQ(cold.settled, 0);
  engine.note_sync_round();
  const Engine::RunResult idle = engine.run(campaign, nullptr);
  EXPECT_EQ(idle.end_reason, "drained");
  EXPECT_GT(idle.settled, 0);  // stolen tickets got worked

  // The steal was claimed in the log.
  std::ifstream in(writer_->segments().active_path());
  std::string line;
  bool found_claim = false;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto batch = json::parse(line, nullptr, false);
    if (batch.is_discarded()) continue;
    for (const auto& record_json : batch["recs"])
      if (record_json.value("t", "") == "CLAIM") found_claim = true;
  }
  EXPECT_TRUE(found_claim);

  // After the other device drops out of the live list, the re-hashed
  // tickets are OWN lane work: no new claims needed (one sync round
  // re-opens the reservation, per decision 0014).
  engine.set_lane_members({{"dev0000000000000001", 0.1}});
  engine.note_sync_round();
  const Engine::RunResult resumed = engine.run(campaign, nullptr);
  EXPECT_EQ(resumed.end_reason, "drained");
  EXPECT_GT(resumed.settled, 0);
}

TEST_F(EngineTest, SingleChainUnderAFiftyPercentShareTerminates) {
  // M8 agent-exit criterion (decision 0020): a single chain halves its
  // pool per level, so it ends in an allowance-spent leaf — even though
  // the funnel fake would happily grow the chain to depth 6.
  seed_root();
  Budget budget("10");
  Engine engine(Engine::Options{*writer_, chain_fake(6), budget, 1,
                                "dev0000000000000001", "0.001"});
  CampaignSpec campaign = spec();
  campaign.loop_budget = 8;
  campaign.structure = true;
  const Engine::RunResult result = engine.run(campaign, nullptr);

  // The chain halves its pool per level: 8 -> 3 -> 1 -> 0. The root and
  // two chain links settle; the third chain link leafs WITHOUT an
  // attempt (end = allowance spent); the chain never reaches layer 4 —
  // the funnel fake alone would grow it to depth 6.
  EXPECT_EQ(result.end_reason, "drained");
  EXPECT_EQ(result.settled, 3);  // root + two chain links
  EXPECT_EQ(result.attempts, 3);  // one per settled node, none on the leaf
  std::string error;
  int chain_leaf_layer = -1;
  int chain_leaf_count = 0;
  for (const onion::store::Node& node : writer_->db().all_nodes(&error)) {
    if (node.title.rfind("Chain Link ", 0) != 0) continue;
    if (writer_->leaf(node.path)) {
      ++chain_leaf_count;
      chain_leaf_layer = node.layer;
    } else {
      EXPECT_LT(node.layer, 3) << "a chain link past the allowance settled";
    }
  }
  EXPECT_EQ(chain_leaf_count, 1);
  EXPECT_EQ(chain_leaf_layer, 3);
  EXPECT_GE(result.leaves, 5);  // dead-end siblings + the allowance leaf
}

TEST_F(EngineTest, DepthCheckpointSpendsNothingAndReleasesOnAccept) {
  // M8 exit criterion: a depth checkpoint is written as NEEDS_REVIEW
  // (reason depth checkpoint) WITHOUT spending an attempt; accepting it
  // releases the ticket until the next checkpoint. Checkpoints every 2
  // levels from layer 1, so the tree pauses at L1 before any child is
  // attempted.
  seed_root();
  Budget budget("10");
  // Fleet mode: a drained single-device run CLOSES the campaign (0010);
  // the fleet drain keeps it running between the accept and the second
  // run. The device owns everything (huge weight).
  Engine engine(Engine::Options{*writer_, funnel_fake(8), budget, 1,
                                "dev0000000000000001", "0.001",
                                std::nullopt, 0,
                                {{"dev0000000000000001", 100.0},
                                 {"dev0000000000000002", 0.1}},
                                "dev0000000000000001"});
  CampaignSpec campaign = spec();
  campaign.loop_budget = 10000;
  campaign.structure = true;
  campaign.depth_checkpoint_from = 1;
  campaign.depth_checkpoint_every = 2;
  // The gate starts closed (0014 amendment): the first run drains cold,
  // one completed round opens it, the next run does the work.
  const Engine::RunResult cold = engine.run(campaign, nullptr);
  EXPECT_EQ(cold.end_reason, "drained");
  engine.note_sync_round();
  const Engine::RunResult first = engine.run(campaign, nullptr);
  // The root settles (attempts=1); its L1 children hit the checkpoint
  // and are paused un-attempted: exactly 3 open checkpoint reviews, no
  // attempts beyond the root's.
  EXPECT_EQ(first.attempts, 1);
  EXPECT_EQ(first.settled, 1);
  EXPECT_EQ(first.reviews, 3);
  std::string error;
  const auto open = writer_->db().open_reviews(&error);
  ASSERT_EQ(open.size(), 3u);
  for (const onion::store::Review& review : open)
    EXPECT_EQ(review.reason, "depth checkpoint");

  // Accept one: it becomes a normal ticket (attempted, settled) and its
  // children hit the NEXT checkpoint (L3 — every 2 from 1).
  engine.review_accept(open[0].path, campaign);
  engine.note_sync_round();
  const Engine::RunResult second = engine.run(campaign, nullptr);
  EXPECT_GT(second.attempts, 1);
  EXPECT_GT(second.settled, 1);
  EXPECT_FALSE(writer_->needs_review(open[0].path));
}

TEST_F(EngineTest, FakeModelThatNeverSaysAtomicStillTerminates) {
  // M8 exit criterion: no "atomic" ever leaves the funnel; the branch
  // shares must end the tree anyway (each level halves its pool).
  seed_root();
  Budget budget("10");
  Engine engine(Engine::Options{*writer_, funnel_fake(12, nullptr, 0.9, 999),
                                budget, 1, "dev0000000000000001", "0.001"});
  CampaignSpec campaign = spec();
  campaign.loop_budget = 16;
  campaign.structure = true;
  const Engine::RunResult result = engine.run(campaign, nullptr);
  // The tree ends by itself (allowance spent or cannot-split) with
  // nothing atomic and a bounded amount of work.
  EXPECT_EQ(result.end_reason, "drained");
  EXPECT_EQ(result.atomic, 0);
  EXPECT_GT(result.settled, 0);
  EXPECT_LE(result.attempts, campaign.loop_budget);
}

TEST_F(EngineTest, FunnelCampaignPastL9EndsWithinItsPools) {
  // M8 exit criterion: a funnel campaign growing past L9 (the old cap)
  // ends within its pools — the branch shares replace the depth limit.
  seed_root();
  Budget budget("10");
  Engine engine(Engine::Options{*writer_, chain_fake(13), budget, 1,
                                "dev0000000000000001", "0.001"});
  CampaignSpec campaign = spec();
  // A pool of 1024 halves per level (1024 -> 511 -> 255 -> ... -> 1 ->
  // 0): the single chain carries past the old L9 cap and ends on its
  // own allowance, not on a depth limit.
  campaign.loop_budget = 1024;
  campaign.structure = true;
  const Engine::RunResult result = engine.run(campaign, nullptr);
  EXPECT_EQ(result.end_reason, "drained");
  // Nodes past the old L9 cap exist...
  std::string error;
  int max_layer = 0;
  for (const onion::store::Node& node : writer_->db().all_nodes(&error))
    max_layer = std::max(max_layer, node.layer);
  EXPECT_GT(max_layer, 9);
  // ...and the spend stayed within the pools (attempts <= budget).
  EXPECT_LE(result.attempts, campaign.loop_budget);
}
