#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>

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

}  // namespace
