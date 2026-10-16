#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "log/log.hpp"
#include "state/state.hpp"
#include "store/store.hpp"
#include "viewcache/viewcache.hpp"

namespace {

using onion::log::NullSigner;
using onion::log::Record;

Record make_record(std::string type, nlohmann::json payload,
                   std::uint64_t hlc = 1) {
  Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  record.hlc = hlc;
  return record;
}

// Folds one ADD_NODE batch through the writer and syncs.
void add_node(onion::state::StateWriter& writer, std::string path,
              int layer, std::string title, std::uint64_t hlc,
              std::string* error) {
  auto record = make_record(
      "ADD_NODE",
      {{"path", path}, {"layer", layer}, {"title", title}}, hlc);
  writer.submit(onion::state::CommitTerminal{{record}});
  writer.sync();
  (void)error;
}

class ViewCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("onion_viewcache_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    StateWriterOptions opts;
    opts.data_dir = dir_;
    opts.origin = "dev0000000000000001";
    writer_ = std::make_unique<onion::state::StateWriter>(std::move(opts));
    std::string error;
    ASSERT_TRUE(writer_->start(&error)) << error;

    onion::viewcache::Cache::Options cache_options;
    cache_options.sidecar_path = dir_ / "viewcache.db";
    cache_options.source_path = dir_ / "onion.db";
    cache_ = std::make_unique<onion::viewcache::Cache>(cache_options);
    ASSERT_TRUE(cache_->start(&error)) << error;
  }
  void TearDown() override {
    if (cache_) cache_->stop();
    if (writer_) writer_->stop();
    // The read-only pool keeps per-thread handles until thread exit, so
    // the remove can fail on Windows while they drain; the runner's temp
    // cleanup reclaims the directory, so the failure is ignored (same
    // policy as the state tests).
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  using StateWriterOptions = onion::state::StateWriter::Options;

  std::filesystem::path dir_;
  std::unique_ptr<onion::state::StateWriter> writer_;
  std::unique_ptr<onion::viewcache::Cache> cache_;
};

}  // namespace

TEST_F(ViewCacheTest, RebuildProducesExactCountsAndSearch) {
  std::string error;
  add_node(*writer_, "P01", 0, "Planning", 1, &error);
  add_node(*writer_, "P01-IN01", 1, "Inventory", 2, &error);
  add_node(*writer_, "P01-IN01-T01", 2, "Task one", 3, &error);
  add_node(*writer_, "P01-IN02", 1, "Other branch", 4, &error);

  ASSERT_TRUE(cache_->rebuild(writer_->read_db(), &error)) << error;

  auto root = cache_->counts_for("P01", &error);
  ASSERT_TRUE(root.has_value());
  EXPECT_EQ(root->children, 2);
  EXPECT_EQ(root->subtree, 3);

  auto in01 = cache_->counts_for("P01-IN01", &error);
  ASSERT_TRUE(in01.has_value());
  EXPECT_EQ(in01->children, 1);
  EXPECT_EQ(in01->subtree, 1);

  auto t01 = cache_->counts_for("P01-IN01-T01", &error);
  ASSERT_TRUE(t01.has_value());
  EXPECT_EQ(t01->children, 0);
  EXPECT_EQ(t01->subtree, 0);

  const auto hits = cache_->search("task*", 10, &error);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].path, "P01-IN01-T01");

  const auto broad = cache_->search("P01*", 10, &error);
  EXPECT_EQ(broad.size(), 4u);
}

TEST_F(ViewCacheTest, StaleTracksTheAppliedVector) {
  std::string error;
  add_node(*writer_, "P01", 0, "Planning", 1, &error);
  ASSERT_TRUE(cache_->rebuild(writer_->read_db(), &error)) << error;

  bool is_stale = true;
  ASSERT_TRUE(cache_->stale(writer_->read_db(), &is_stale, &error)) << error;
  EXPECT_FALSE(is_stale);

  add_node(*writer_, "P01-IN01", 1, "Inventory", 2, &error);
  ASSERT_TRUE(cache_->stale(writer_->read_db(), &is_stale, &error)) << error;
  EXPECT_TRUE(is_stale);
}

TEST_F(ViewCacheTest, IncrementalUpdateMatchesARebuild) {
  std::string error;
  add_node(*writer_, "P01", 0, "Planning", 1, &error);
  add_node(*writer_, "P01-IN01", 1, "Inventory", 2, &error);
  ASSERT_TRUE(cache_->rebuild(writer_->read_db(), &error)) << error;

  // A new node arrives: fold it, then apply the incremental path.
  add_node(*writer_, "P01-IN01-T01", 2, "Task one", 3, &error);
  ASSERT_TRUE(cache_->node_added("P01-IN01-T01", "Task one", &error))
      << error;

  auto root = cache_->counts_for("P01", &error);
  ASSERT_TRUE(root.has_value());
  EXPECT_EQ(root->children, 1);
  EXPECT_EQ(root->subtree, 2);

  auto in01 = cache_->counts_for("P01-IN01", &error);
  ASSERT_TRUE(in01.has_value());
  EXPECT_EQ(in01->children, 1);
  EXPECT_EQ(in01->subtree, 1);

  // A full rebuild must land on the same numbers.
  ASSERT_TRUE(cache_->rebuild(writer_->read_db(), &error)) << error;
  auto rebuilt_root = cache_->counts_for("P01", &error);
  ASSERT_TRUE(rebuilt_root.has_value());
  EXPECT_EQ(rebuilt_root->children, root->children);
  EXPECT_EQ(rebuilt_root->subtree, root->subtree);
  const auto hits = cache_->search("task*", 10, &error);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].path, "P01-IN01-T01");
}

TEST_F(ViewCacheTest, TombstoneDecrementsCountsAndSearch) {
  std::string error;
  add_node(*writer_, "P01", 0, "Planning", 1, &error);
  add_node(*writer_, "P01-IN01", 1, "Inventory", 2, &error);
  ASSERT_TRUE(cache_->rebuild(writer_->read_db(), &error)) << error;

  auto tombstone =
      make_record("TOMBSTONE", {{"path", "P01-IN01"}}, 3);
  writer_->submit(onion::state::CommitTerminal{{tombstone}});
  writer_->sync();

  ASSERT_TRUE(cache_->node_removed("P01-IN01", &error)) << error;
  auto root = cache_->counts_for("P01", &error);
  ASSERT_TRUE(root.has_value());
  EXPECT_EQ(root->children, 0);   // the tombstoned child is no longer live
  EXPECT_EQ(root->subtree, 0);    // and its subtree is gone with it
}
