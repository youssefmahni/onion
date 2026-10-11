#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "state/state.hpp"

namespace {

using onion::log::Record;
using onion::state::AppendAttempt;
using onion::state::CommitTerminal;
using onion::state::DevImport;
using onion::state::DevImportedNode;
using onion::state::StateWriter;

Record make_record(std::string type, nlohmann::json payload) {
  Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  return record;
}

class StateWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("onion_state_test_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    StateWriter::Options options;
    options.data_dir = dir_;
    options.origin = "dev0000000000000001";
    writer_ = std::make_unique<StateWriter>(std::move(options));
  }
  void TearDown() override {
    if (writer_) writer_->stop();
    std::filesystem::remove_all(dir_);
  }

  std::filesystem::path dir_;
  std::unique_ptr<StateWriter> writer_;
};

TEST_F(StateWriterTest, TerminalBatchCommitsDurablyAndFolds) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;

  std::vector<Record> records = {
      make_record("ADD_NODE", {{"path", "P01-IN01-T01-TK00001"},
                               {"layer", 4},
                               {"title", "Order Capture"}}),
      make_record("CRITIQUE", {{"path", "P01-IN01-T01"},
                               {"score", "0.85"},
                               {"issues", ""}}),
      make_record("SETTLE", {{"path", "P01-IN01-T01"},
                             {"kept", 1},
                             {"atomic", 0},
                             {"attempts", 1},
                             {"score", "0.85"},
                             {"end", "children"},
                             {"campaign", "c1"}})};
  writer_->submit(CommitTerminal{std::move(records)});
  writer_->sync();

  EXPECT_TRUE(writer_->settled("P01-IN01-T01"));
  EXPECT_FALSE(writer_->leaf("P01-IN01-T01"));

  std::string query_error;
  const auto node = writer_->db().node_by_path("P01-IN01-T01-TK00001", &query_error);
  ASSERT_TRUE(node.has_value()) << query_error;
  EXPECT_EQ(node->title, "Order Capture");
  EXPECT_EQ(node->layer, 4);

  // The batch is one JSON line in the segment with a durable sync.
  std::ifstream in(writer_->segments().active_path());
  std::string line;
  std::getline(in, line);
  EXPECT_NE(line.find("\"SETTLE\""), std::string::npos);
  EXPECT_EQ(writer_->segments().total_batches(), 1u);
}

TEST_F(StateWriterTest, AttemptsGroupCommitIntoOneBatch) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;

  writer_->submit(AppendAttempt{
      make_record("ATTEMPT", {{"path", "P01-IN01-T01"}, {"n", 1},
                              {"score", "0.4"}, {"passed", 0},
                              {"tokens_in", 10}, {"tokens_cached", 0},
                              {"tokens_out", 20}, {"cost", "0.001"},
                              {"campaign", "c1"}, {"issues", ""}})});
  writer_->submit(AppendAttempt{
      make_record("ATTEMPT", {{"path", "P01-IN01-T02"}, {"n", 1},
                              {"score", "0.6"}, {"passed", 1},
                              {"tokens_in", 11}, {"tokens_cached", 1},
                              {"tokens_out", 21}, {"cost", "0.002"},
                              {"campaign", "c1"}, {"issues", ""}})});
  writer_->sync();

  EXPECT_EQ(writer_->segments().total_batches(), 1u);
  EXPECT_EQ(writer_->next_batch(), 2u);
  EXPECT_EQ(writer_->next_seq(), 3u);

  std::string query_error;
  EXPECT_NE(writer_->db().spend_usd(&query_error), "0");
}

TEST_F(StateWriterTest, LeafAndReviewIndexing) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;

  writer_->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01-T01"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer_->submit(CommitTerminal{{
      make_record("NEEDS_REVIEW", {{"path", "P01-IN01-T02"},
                                   {"reason", "below floor"}}),
  }});
  writer_->sync();

  EXPECT_TRUE(writer_->settled("P01-IN01-T01"));
  EXPECT_TRUE(writer_->leaf("P01-IN01-T01"));
  EXPECT_TRUE(writer_->needs_review("P01-IN01-T02"));
}

TEST_F(StateWriterTest, RestartReplaysLogAndRestoresIndex) {
  std::string error;
  auto open_writer = [this]() {
    StateWriter::Options options;
    options.data_dir = dir_;
    options.origin = "dev0000000000000001";
    return std::make_unique<StateWriter>(std::move(options));
  };
  {
    auto writer = open_writer();
    ASSERT_TRUE(writer->start(&error)) << error;
    writer->submit(CommitTerminal{{
        make_record("ADD_NODE", {{"path", "P01-IN01-T01-TK00001"},
                                 {"layer", 4},
                                 {"title", "Survives Restart"}}),
        make_record("SETTLE", {{"path", "P01-IN01-T01"}, {"kept", 1},
                               {"end", "children"}}),
    }});
    writer->sync();
    writer->stop();
  }
  {
    auto writer = open_writer();
    ASSERT_TRUE(writer->start(&error)) << error;
    EXPECT_TRUE(writer->settled("P01-IN01-T01"));
    EXPECT_EQ(writer->next_seq(), 3u);
    EXPECT_EQ(writer->next_batch(), 2u);
    std::string query_error;
    const auto node = writer->db().node_by_path("P01-IN01-T01-TK00001", &query_error);
    ASSERT_TRUE(node.has_value()) << query_error;
    EXPECT_EQ(node->title, "Survives Restart");
    writer->stop();
  }
}

TEST_F(StateWriterTest, CampaignFoldAndStopIsFinal) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;

  writer_->submit(CommitTerminal{{
      make_record("CAMPAIGN", {{"id", "c1"}, {"spec", {{"model", "deepseek-flash"}}},
                               {"state", "running"}}),
  }});
  writer_->sync();
  ASSERT_TRUE(writer_->campaign("c1").has_value());
  EXPECT_EQ(writer_->campaign("c1")->state, "running");

  writer_->submit(CommitTerminal{{
      make_record("CAMPAIGN", {{"id", "c1"}, {"spec", {{"model", "deepseek-flash"}}},
                               {"state", "stopped"}}),
  }});
  writer_->sync();
  EXPECT_EQ(writer_->campaign("c1")->state, "stopped");

  // A later stale state must not resurrect a stopped campaign.
  writer_->submit(CommitTerminal{{
      make_record("CAMPAIGN", {{"id", "c1"}, {"spec", {{"model", "deepseek-flash"}}},
                               {"state", "paused"}}),
  }});
  writer_->sync();
  EXPECT_EQ(writer_->campaign("c1")->state, "stopped");
}

TEST_F(StateWriterTest, DevImportWritesStoreWithoutLog) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;
  const std::uint64_t batches_before = writer_->segments().total_batches();

  DevImport import;
  import.nodes.push_back({"P01", "P01", 0, "Phase"});
  import.nodes.push_back({"IN01", "P01-IN01", 1, "In"});
  writer_->submit(std::move(import));
  writer_->sync();

  EXPECT_EQ(writer_->segments().total_batches(), batches_before);
  std::string query_error;
  const auto subtree = writer_->db().nodes_in_subtree("P01", &query_error);
  ASSERT_EQ(subtree.size(), 2u);
}

// Crash injection at each commit step: the hook is armed inside a forked
// child, which dies at the step with _exit — a real power cut. The parent
// then reopens the data directory and checks the replay.
#ifndef _WIN32
namespace {
int run_writer_until_crash(const std::filesystem::path& dir, int crash_step) {
  const pid_t child = fork();
  if (child != 0) {
    int status = 0;
    waitpid(child, &status, 0);
    return status;
  }
  StateWriter::Options options;
  options.data_dir = dir;
  options.origin = "dev0000000000000001";
  options.crash_hook = [crash_step](int step) {
    if (step == crash_step) _exit(3);
  };
  auto writer = std::make_unique<StateWriter>(std::move(options));
  std::string error;
  if (!writer->start(&error)) _exit(4);
  writer->submit(CommitTerminal{{
      Record{0, 0, "ADD_NODE",
             {{"path", "P01-IN01-T01-TK00001"}, {"layer", 4},
              {"title", "Committed"}}},
      Record{0, 0, "SETTLE",
             {{"path", "P01-IN01-T01"}, {"kept", 1}, {"end", "children"}}},
  }});
  // The loop is only a timeout guard: the crash hook kills the child at the
  // right step.  Polling `next_batch()` here would race the hook, because
  // the counter increments before the durable write and the fold
  // (append_batch), which produced spurious exit-5 failures under load.
  for (int i = 0; i < 3000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  _exit(5);  // the hook never fired: test failure
}
}  // namespace

TEST_F(StateWriterTest, CrashBetweenDurableWriteAndFoldLosesNothing) {
  writer_.reset();  // release the fixture writer before the fork
  const int status = run_writer_until_crash(dir_, 1);
  EXPECT_EQ(WEXITSTATUS(status), 3);

  StateWriter::Options options;
  options.data_dir = dir_;
  options.origin = "dev0000000000000001";
  auto writer = std::make_unique<StateWriter>(std::move(options));
  std::string error;
  ASSERT_TRUE(writer->start(&error)) << error;
  EXPECT_TRUE(writer->settled("P01-IN01-T01"));
  std::string query_error;
  const auto node = writer->db().node_by_path("P01-IN01-T01-TK00001", &query_error);
  ASSERT_TRUE(node.has_value()) << query_error;
  EXPECT_EQ(node->title, "Committed");
  writer->stop();
}

TEST_F(StateWriterTest, CrashAfterFoldKeepsStoreAndLogConsistent) {
  writer_.reset();
  const int status = run_writer_until_crash(dir_, 2);
  EXPECT_EQ(WEXITSTATUS(status), 3);

  StateWriter::Options options;
  options.data_dir = dir_;
  options.origin = "dev0000000000000001";
  auto writer = std::make_unique<StateWriter>(std::move(options));
  std::string error;
  ASSERT_TRUE(writer->start(&error)) << error;
  EXPECT_TRUE(writer->settled("P01-IN01-T01"));
  std::string query_error;
  const auto node = writer->db().node_by_path("P01-IN01-T01-TK00001", &query_error);
  ASSERT_TRUE(node.has_value()) << query_error;
  EXPECT_EQ(node->title, "Committed");
  // The idempotent replay does not duplicate the node: the subtree holds
  // exactly the one committed child (the parent itself has no node row).
  const auto subtree = writer->db().nodes_in_subtree("P01-IN01-T01", &query_error);
  EXPECT_EQ(subtree.size(), 1u);
  writer->stop();
}
#endif  // !_WIN32

}  // namespace

// ----------------------------------------------------- M4: signed replay

namespace {
std::vector<std::uint8_t> seed_00_1f() {
  std::vector<std::uint8_t> seed(32);
  for (std::size_t i = 0; i < seed.size(); ++i)
    seed[i] = static_cast<std::uint8_t>(i);
  return seed;
}
}  // namespace

TEST_F(StateWriterTest, SignedWriterChainsFromThePublicKeyHash) {
  writer_.reset();
  auto key = std::make_shared<onion::log::DeviceKey>(
      onion::log::DeviceKey::from_seed(seed_00_1f()));
  StateWriter::Options options;
  options.data_dir = dir_;
  options.origin = key->device_id;
  options.device_key = key;
  options.signer = std::make_unique<onion::log::Ed25519Signer>(*key);
  auto writer = std::make_unique<StateWriter>(std::move(options));
  std::string error;
  ASSERT_TRUE(writer->start(&error)) << error;
  writer->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer->sync();
  // Batch 1's prev is the public-key hash, not empty.
  std::ifstream in(writer->segments().active_path());
  std::string line;
  std::getline(in, line);
  auto batch = nlohmann::json::parse(line);
  EXPECT_EQ(batch["prev"].get<std::string>(), onion::log::genesis_prev(*key));
  EXPECT_EQ(batch["o"].get<std::string>(), key->device_id);
  EXPECT_EQ(batch["sig"].get<std::string>().size(), 128u);  // 64 bytes hex
  writer->stop();
}

TEST_F(StateWriterTest, TornTailAtEveryByteOffsetKeepsCompleteBatches) {
  writer_.reset();
  auto key = std::make_shared<onion::log::DeviceKey>(
      onion::log::DeviceKey::from_seed(seed_00_1f()));
  const auto open_signed = [&]() {
    StateWriter::Options options;
    options.data_dir = dir_;
    options.origin = key->device_id;
    options.device_key = key;
    options.signer = std::make_unique<onion::log::Ed25519Signer>(*key);
    return std::make_unique<StateWriter>(std::move(options));
  };
  const auto settled_record = [](std::string path, onion::log::Hlc hlc) {
    Record record;
    record.type = "SETTLE";
    record.payload = {{"path", path}, {"kept", 0}, {"end", "cannot split"}};
    record.hlc = hlc;  // deterministic: identical bytes across runs
    return record;
  };

  // Write two batches with fixed HLCs and capture the segment bytes.
  std::string content;
  {
    auto writer = open_signed();
    std::string error;
    ASSERT_TRUE(writer->start(&error)) << error;
    writer->submit(CommitTerminal{{
        settled_record("P01-IN01", 1001),
    }});
    writer->sync();
    writer->submit(CommitTerminal{{
        settled_record("P01-IN02", 1002),
    }});
    writer->sync();
    std::ifstream in(writer->segments().active_path(), std::ios::binary);
    content.assign((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
    writer->stop();
  }
  const std::size_t second_line_start = content.find('\n') + 1;
  ASSERT_LT(second_line_start, content.size());
  const std::size_t second_line_length = content.size() - second_line_start;

  // A torn write never reaches the store: simulate a crash at every byte
  // offset of the last batch by truncating the log there and rebuilding the
  // store from the surviving bytes (the corrupted-SQLite recovery path).
  for (std::size_t cut = 0; cut < second_line_length; ++cut) {
    std::error_code ec;
    std::filesystem::remove(dir_ / "onion.db", ec);
    std::filesystem::remove(dir_ / "onion.db-wal", ec);
    std::filesystem::remove(dir_ / "onion.db-shm", ec);
    {
      const std::filesystem::path segment =
          dir_ / "oplog" / key->device_id / "1.log";
      std::ofstream truncated(segment, std::ios::binary | std::ios::trunc);
      truncated.write(content.data(), static_cast<std::streamsize>(
                                          second_line_start + cut));
      truncated.close();
    }
    auto writer = open_signed();
    std::string error;
    ASSERT_TRUE(writer->start(&error)) << error << " at cut " << cut;
    EXPECT_TRUE(writer->settled("P01-IN01")) << "at cut " << cut;
    EXPECT_FALSE(writer->settled("P01-IN02")) << "at cut " << cut;
    EXPECT_EQ(writer->next_batch(), 2u) << "at cut " << cut;
    EXPECT_EQ(writer->segments().total_batches(), 1u) << "at cut " << cut;
    writer->stop();
  }
}

TEST_F(StateWriterTest, SealedSegmentsReplayAcrossRestart) {
  writer_.reset();
  auto key = std::make_shared<onion::log::DeviceKey>(
      onion::log::DeviceKey::from_seed(seed_00_1f()));
  const auto open_signed = [&](std::uint64_t seal_bytes = 64u * 1024u * 1024u) {
    StateWriter::Options options;
    options.data_dir = dir_;
    options.origin = key->device_id;
    options.device_key = key;
    options.signer = std::make_unique<onion::log::Ed25519Signer>(*key);
    options.seal_bytes = seal_bytes;
    return std::make_unique<StateWriter>(std::move(options));
  };
  // A small seal threshold forces a rotation: the second batch seals
  // segment 1 and lands in segment 2.
  {
    auto writer = open_signed(400);
    std::string error;
    ASSERT_TRUE(writer->start(&error)) << error;
    writer->submit(CommitTerminal{{
        make_record("SETTLE", {{"path", "P01-IN01"}, {"kept", 0},
                               {"end", "cannot split"}}),
    }});
    writer->sync();
    writer->submit(CommitTerminal{{
        make_record("SETTLE", {{"path", "P01-IN02"}, {"kept", 0},
                               {"end", "cannot split"}}),
    }});
    writer->sync();
    writer->stop();
  }
  {
    auto writer = open_signed();
    std::string error;
    ASSERT_TRUE(writer->start(&error)) << error;
    EXPECT_TRUE(writer->settled("P01-IN01"));
    EXPECT_TRUE(writer->settled("P01-IN02"));
    EXPECT_EQ(writer->next_batch(), 3u);
    EXPECT_EQ(writer->segments().sealed_first_seqs().size(), 1u);
    writer->stop();
  }
}

// -------------------------------------------------- M6 conflict table (fold)

namespace {
using nlohmann::json;

Record settle_record(std::string path, double score, std::uint64_t hlc,
                     std::string origin, int attempts = 1) {
  Record record;
  record.type = "SETTLE";
  record.payload = {{"path", path},
                    {"kept", 1},
                    {"atomic", 0},
                    {"attempts", attempts},
                    {"score", std::to_string(score)},
                    {"end", "children"},
                    {"campaign", "c1"},
                    {"origin", origin}};
  record.hlc = hlc;
  return record;
}
}  // namespace

TEST_F(StateWriterTest, ConflictHigherScoreWinsInEitherOrder) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;
  // Order A: low score first, high score second.
  writer_->submit(CommitTerminal{{settle_record("P01", 0.6, 10, "dev0000000000000001")}});
  writer_->submit(CommitTerminal{{settle_record("P01", 0.9, 11, "dev0000000000000001")}});
  writer_->sync();
  auto settles = writer_->db().settles(&error);
  ASSERT_EQ(settles.size(), 1u);
  EXPECT_EQ(settles[0].score, "0.900000");
}

TEST_F(StateWriterTest, ConflictHigherScoreWinsInReverseOrder) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;
  // Order B: high score first, low score second — the fold must still keep
  // the higher score (the reverse order must give the same state).
  writer_->submit(CommitTerminal{{settle_record("P02", 0.9, 11, "dev0000000000000001")}});
  writer_->submit(CommitTerminal{{settle_record("P02", 0.6, 10, "dev0000000000000001")}});
  writer_->sync();
  auto settles = writer_->db().settles(&error);
  ASSERT_EQ(settles.size(), 1u);
  EXPECT_EQ(settles[0].score, "0.900000");
}

TEST_F(StateWriterTest, ConflictTieBreaksOnHighestHlcThenOrigin) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;
  // Same hlc: the higher origin wins.
  writer_->submit(CommitTerminal{{settle_record("P03", 0.8, 10, "aaaaaaaaaaaaaaaa")}});
  writer_->submit(CommitTerminal{{settle_record("P03", 0.7, 10, "bbbbbbbbbbbbbbbb")}});
  writer_->sync();
  auto settles = writer_->db().settles(&error);
  ASSERT_EQ(settles.size(), 1u);
  EXPECT_EQ(settles[0].origin, "bbbbbbbbbbbbbbbb");
  // Reverse order: the same state.
  writer_->submit(CommitTerminal{{settle_record("P04", 0.7, 10, "bbbbbbbbbbbbbbbb")}});
  writer_->submit(CommitTerminal{{settle_record("P04", 0.8, 10, "aaaaaaaaaaaaaaaa")}});
  writer_->sync();
  settles = writer_->db().settles(&error);
  ASSERT_EQ(settles.size(), 2u);
  const auto p04 = writer_->db().node_by_path("P04", &error);
  (void)p04;
}

TEST_F(StateWriterTest, CampaignStopFinalInEitherOrder) {
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;
  writer_->submit(CommitTerminal{{make_record(
      "CAMPAIGN", {{"id", "c1"}, {"spec", json::object()}, {"state", "stopped"}})}});
  writer_->submit(CommitTerminal{{make_record(
      "CAMPAIGN", {{"id", "c1"}, {"spec", json::object()}, {"state", "paused"}})}});
  writer_->sync();
  EXPECT_EQ(writer_->campaign("c1")->state, "stopped");
  // Reverse: a resume after a stop still stays stopped.
  writer_->submit(CommitTerminal{{make_record(
      "CAMPAIGN", {{"id", "c2"}, {"spec", json::object()}, {"state", "running"}})}});
  writer_->submit(CommitTerminal{{make_record(
      "CAMPAIGN", {{"id", "c2"}, {"spec", json::object()}, {"state", "stopped"}})}});
  writer_->sync();
  EXPECT_EQ(writer_->campaign("c2")->state, "stopped");
}

namespace {

onion::log::Batch reserve_batch(
    const std::string& origin, std::uint64_t number,
    std::vector<std::pair<std::int64_t, std::int64_t>> reserves) {
  std::vector<Record> records;
  std::int64_t seq = 1;  // the replay's contiguity check expects 1..n
  for (const auto& [hlc, attempts] : reserves) {
    Record record = make_record(
        "RESERVE", {{"campaign", "mp1"}, {"attempts", attempts}});
    record.hlc = hlc;
    record.seq = seq++;
    records.push_back(std::move(record));
  }
  onion::log::NullSigner signer;
  return onion::log::make_batch(origin, number, std::string(64, '0'),
                                std::move(records), signer);
}

void submit_campaign(StateWriter& writer) {
  writer.submit(CommitTerminal{{make_record(
      "CAMPAIGN", {{"id", "mp1"},
                   {"spec", {{"loop_budget", 1000}, {"chunk", 50}}},
                   {"state", "running"}})}});
  writer.sync();
}

}  // namespace

TEST_F(StateWriterTest, CappedFoldVoidsPastBudgetInEveryDeliveryOrder) {
  // Three origins reserve 500 attempts each (10 chunks of 50) into a
  // budget of 1000: the fold counts in (hlc, origin) order, caps at the
  // budget and voids the rest — identically in EVERY delivery order
  // (decision 0014).
  const std::string origin_a = "dev00000000000000aa";
  const std::string origin_b = "dev00000000000000bb";
  const std::string origin_c = "dev00000000000000cc";
  const auto reserves_for = [](std::int64_t base_hlc) {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    for (int i = 0; i < 10; i++) out.emplace_back(base_hlc + i, 50);
    return out;
  };
  const onion::log::Batch batch_a =
      reserve_batch(origin_a, 1, reserves_for(1000));
  const onion::log::Batch batch_b =
      reserve_batch(origin_b, 1, reserves_for(2000));
  const onion::log::Batch batch_c =
      reserve_batch(origin_c, 1, reserves_for(3000));

  const std::vector<std::vector<const onion::log::Batch*>> orders = {
      {&batch_a, &batch_b, &batch_c},
      {&batch_a, &batch_c, &batch_b},
      {&batch_b, &batch_a, &batch_c},
      {&batch_b, &batch_c, &batch_a},
      {&batch_c, &batch_a, &batch_b},
      {&batch_c, &batch_b, &batch_a},
  };

  int order_index = 0;
  for (const auto& order : orders) {
    const std::filesystem::path fresh =
        dir_ / ("order_" + std::to_string(order_index++));
    StateWriter::Options options;
    options.data_dir = fresh;
    options.origin = "dev0000000000000001";
    StateWriter writer(std::move(options));
    std::string error;
    ASSERT_TRUE(writer.start(&error)) << error;
    submit_campaign(writer);
    for (const auto* batch : order) {
      writer.submit(onion::state::AppendForeign{*batch});
      writer.sync();
    }

    // Capped at the budget regardless of the delivery order.
    EXPECT_EQ(writer.reserved_total("mp1"), 1000);

    // The hlc ranges do not overlap, so the (hlc, origin) order groups
    // them: A's 10 (500) + B's 10 (500) exactly reach the cap; every C
    // entry is void.
    EXPECT_FALSE(writer.reservation_void("mp1", origin_a, 1000));
    EXPECT_FALSE(writer.reservation_void("mp1", origin_a, 1009));
    EXPECT_FALSE(writer.reservation_void("mp1", origin_b, 2000));
    EXPECT_FALSE(writer.reservation_void("mp1", origin_b, 2009));
    EXPECT_TRUE(writer.reservation_void("mp1", origin_c, 3000));
    EXPECT_TRUE(writer.reservation_void("mp1", origin_c, 3009));
    writer.stop();
  }
}

TEST_F(StateWriterTest, ReservationThatExactlyFitsIsNotVoid) {
  const std::string origin_a = "dev00000000000000aa";
  const std::string origin_b = "dev00000000000000bb";
  std::string start_error;
  ASSERT_TRUE(writer_->start(&start_error)) << start_error;
  submit_campaign(*writer_);
  writer_->submit(onion::state::AppendForeign{reserve_batch(origin_a, 1,
                                              {{100, 500}, {200, 500}})});
  writer_->sync();
  // Exactly 1000 of 1000: the second entry fits precisely.
  EXPECT_EQ(writer_->reserved_total("mp1"), 1000);
  EXPECT_FALSE(writer_->reservation_void("mp1", origin_a, 200));

  // One attempt over: the crossing entry is void and nothing beyond it
  // counts.
  writer_->submit(onion::state::AppendForeign{reserve_batch(origin_b, 1, {{300, 50}})});
  writer_->sync();
  EXPECT_EQ(writer_->reserved_total("mp1"), 1000);
  EXPECT_TRUE(writer_->reservation_void("mp1", origin_b, 300));
}

TEST_F(StateWriterTest, ReserveFoldSurvivesRestart) {
  // Decision 0014: the RESERVE fold is log-only (no store table) — after
  // a restart the replay must restore the capped total and the void set,
  // or a rebooted device forgets the fleet's budget accounting.
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;
  submit_campaign(*writer_);
  writer_->submit(onion::state::AppendForeign{reserve_batch(
      "dev00000000000000aa", 1, {{100, 500}, {200, 500}})});
  writer_->sync();
  EXPECT_EQ(writer_->reserved_total("mp1"), 1000);

  // Restart on the same directory.
  writer_->stop();
  StateWriter::Options options;
  options.data_dir = dir_;
  options.origin = "dev0000000000000001";
  writer_ = std::make_unique<StateWriter>(std::move(options));
  ASSERT_TRUE(writer_->start(&error)) << error;
  EXPECT_EQ(writer_->reserved_total("mp1"), 1000);
  EXPECT_FALSE(writer_->reservation_void("mp1", "dev00000000000000aa", 100));
  EXPECT_FALSE(writer_->reservation_void("mp1", "dev00000000000000aa", 200));
}

TEST_F(StateWriterTest, StaleCampaignRecordDoesNotMoveTheBudget) {
  // Latest (hlc, origin) wins for the budget too: a stale CAMPAIGN record
  // delivered late must not shrink the cap on any replica.
  std::string error;
  ASSERT_TRUE(writer_->start(&error)) << error;
  submit_campaign(*writer_);  // budget 1000, folded with a real hlc

  // A stale record with a LOWER hlc claims budget 10.
  onion::log::Record stale;
  stale.type = "CAMPAIGN";
  stale.payload = {{"id", "mp1"},
                   {"spec", {{"loop_budget", 10}, {"chunk", 50}}},
                   {"state", "running"}};
  stale.hlc = 1;  // far below the first record's hlc
  onion::log::NullSigner signer;
  writer_->submit(onion::state::AppendForeign{onion::log::make_batch(
      "dev00000000000000bb", 1, std::string(64, '0'),
      std::vector<onion::log::Record>{stale}, signer)});
  writer_->sync();

  writer_->submit(onion::state::AppendForeign{reserve_batch(
      "dev00000000000000aa", 1, {{100, 500}, {200, 500}})});
  writer_->sync();
  // The cap stayed 1000: both entries count.
  EXPECT_EQ(writer_->reserved_total("mp1"), 1000);
  EXPECT_FALSE(writer_->reservation_void("mp1", "dev00000000000000aa", 200));
}
