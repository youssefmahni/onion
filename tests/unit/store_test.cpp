#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>

#include "log/log.hpp"
#include "store/store.hpp"
#include "store/checkpoint.hpp"

namespace {

using onion::store::Db;
using onion::store::Node;

class StoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("onion_store_test_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
    db_ = std::make_unique<Db>(dir_ / "onion.db");
    std::string error;
    ASSERT_TRUE(db_->open(&error)) << error;
  }
  void TearDown() override {
    db_->close();
    db_.reset();
    std::filesystem::remove_all(dir_);
  }

  void insert_node(std::string code, std::string path, int layer,
                   std::string title, std::int64_t parent_id = -1) {
    std::string error;
    std::string sql = "INSERT INTO nodes(id, code, path, parent_id, layer, "
                      "title, origin, hlc) VALUES (";
    sql += std::to_string(next_id_++);
    sql += ", '" + code + "', '" + path + "', ";
    if (parent_id >= 0) sql += std::to_string(parent_id);
    else sql += "NULL";
    sql += ", " + std::to_string(layer) + ", '" + title +
           "', 'dev', 1);";
    ASSERT_TRUE(db().exec(sql, &error)) << error;
  }

  Db& db() { return *db_; }

  std::unique_ptr<Db> db_;
  std::filesystem::path dir_;
  std::int64_t next_id_{1};
};

TEST_F(StoreTest, OpensWalModeAndStampsSchemaVersion) {
  std::string error;
  const auto rows = db().exec(
      "SELECT * FROM schema_version;", &error);
  EXPECT_TRUE(rows) << error;
  // WAL mode leaves a -wal file after the first write.
  ASSERT_TRUE(db().exec("INSERT INTO campaigns(id, spec, state, hlc, origin) "
                       "VALUES ('c1','{}','running',1,'dev');",
                       &error))
      << error;
  EXPECT_TRUE(std::filesystem::exists(dir_ / "onion.db-wal"));
}

TEST_F(StoreTest, SubtreeQueryNeverCrossesSiblingPrefixes) {
  // "T01" must never match "T010": the range floor is "T01-" not "T01".
  insert_node("IN01", "P01-IN01", 1, "In");
  insert_node("T01", "P01-IN01-T01", 2, "One");
  insert_node("T010", "P01-IN01-T010", 2, "Ten");
  insert_node("T01-ST01", "P01-IN01-T01-ST01", 3, "One Sub");
  std::string error;
  const auto subtree = db().nodes_in_subtree("P01-IN01-T01", &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(subtree.size(), 2u);
  EXPECT_EQ(subtree[0].path, "P01-IN01-T01");
  EXPECT_EQ(subtree[1].path, "P01-IN01-T01-ST01");
}

TEST_F(StoreTest, LayerAndPrefixQueries) {
  insert_node("P01", "P01", 0, "Phase");
  insert_node("P02", "P02", 0, "Phase 2");
  insert_node("IN01", "P01-IN01", 1, "In");
  insert_node("IN02", "P01-IN02", 1, "In 2");
  std::string error;
  const auto layer0 = db().nodes_at_layer(0, &error);
  ASSERT_EQ(layer0.size(), 2u);
  const auto prefix = db().nodes_with_path_prefix("P01", &error);
  ASSERT_EQ(prefix.size(), 3u);  // P01, P01-IN01, P01-IN02
}

TEST_F(StoreTest, CsvExportMatchesLegacyShape) {
  insert_node("P01", "P01", 0, "Phase");
  insert_node("IN01", "P01-IN01", 1, "In, With \"Comma\"");
  std::string error;
  ASSERT_TRUE(db().export_csv(dir_ / "export", 1, &error)) << error;

  std::ifstream level0(dir_ / "export" / "Onion - level_00.csv");
  std::string line;
  std::getline(level0, line);
  EXPECT_EQ(line, "code,title");
  std::getline(level0, line);
  EXPECT_EQ(line, "P01,Phase");

  std::ifstream level1(dir_ / "export" / "Onion - level_01.csv");
  std::getline(level1, line);
  EXPECT_EQ(line, "code,path,title");
  std::getline(level1, line);
  EXPECT_EQ(line, "IN01,P01-IN01,\"In, With \"\"Comma\"\"\"");
}

TEST_F(StoreTest, RejectsNewerSchema) {
  std::string error;
  ASSERT_TRUE(db().exec("UPDATE schema_version SET version = 99;", &error)) << error;
  // A second open must refuse: the store was written by a newer build.
  Db second(dir_ / "onion.db");
  EXPECT_FALSE(second.open(&error));
  EXPECT_NE(error.find("newer"), std::string::npos) << error;
}

TEST_F(StoreTest, SettlesAndSpendQueries) {
  std::string error;
  ASSERT_TRUE(db().exec(
      "INSERT INTO settles(path, kept, atomic, attempts, score, campaign, "
      "hlc, origin) VALUES ('P01-IN01-T01', 5, 2, 1, '0.85', 'c1', 10, 'dev');",
      &error))
      << error;
  ASSERT_TRUE(db().exec(
      "INSERT INTO attempts(path, n, score, passed, tokens_in, tokens_cached, "
      "tokens_out, cost, campaign, hlc, origin) VALUES "
      "('P01-IN01-T01', 1, '0.85', 1, 100, 50, 40, '0.0012', 'c1', 9, 'dev');",
      &error))
      << error;
  const auto settles = db().settles(&error);
  ASSERT_EQ(settles.size(), 1u);
  EXPECT_EQ(settles[0].kept, 5);
  EXPECT_EQ(db().spend_usd(&error), "0.0012");
}

TEST_F(StoreTest, ReportsRecentSpendRateFromHlcWallTime) {
  const std::int64_t now_hlc =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count()
      << 16;
  std::string error;
  ASSERT_TRUE(db().exec(
      "INSERT INTO attempts(path,n,score,passed,tokens_in,tokens_cached,"
      "tokens_out,cost,campaign,hlc,origin) VALUES "
      "('P01',1,'0.8',1,0,0,0,'1.5','c'," +
          std::to_string(now_hlc) + ",'dev');",
      &error)) << error;
  EXPECT_EQ(db().spend_rate_usd_per_hour(3600, &error), "1.5");
}

}  // namespace

// ------------------------------------------------------------- checkpoints

TEST(CheckpointTest, MerkleRootIsStableAndOrderIndependent) {
  std::vector<Node> nodes;
  Node a; a.path = "P01"; a.layer = 0; a.title = "Phase"; a.hlc = 1; a.code = "P01";
  Node b; b.path = "P01-IN01"; b.layer = 1; b.title = "In"; b.hlc = 2; b.code = "IN01";
  nodes.push_back(b);
  nodes.push_back(a);  // unsorted on purpose
  const std::string root = onion::store::merkle_root(nodes);
  EXPECT_EQ(root.size(), 64u);
  std::swap(nodes[0], nodes[1]);
  EXPECT_EQ(onion::store::merkle_root(nodes), root);
  // A different title changes the root.
  nodes[1].title = "Different";
  EXPECT_NE(onion::store::merkle_root(nodes), root);
}

TEST(CheckpointTest, WritesSnapshotManifestAndKeepsThree) {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("onion_checkpoint_test_" +
                    std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count()));
  std::filesystem::create_directories(dir);
  {
    Db db(dir / "onion.db");
    std::string error;
    ASSERT_TRUE(db.open(&error)) << error;
    ASSERT_TRUE(db.exec(
        "INSERT INTO nodes(id, code, path, layer, title, origin, hlc) "
        "VALUES (1, 'P01', 'P01', 0, 'Phase', 'dev', 1);",
        &error))
        << error;
    ASSERT_TRUE(db.exec(
        "INSERT INTO applied(origin, seq) VALUES ('dev0000000000000001', 5);",
        &error))
        << error;
  }
  onion::store::CheckpointWriter writer(dir, [](std::string_view bytes) {
    return onion::log::blake2b_hex(bytes);  // any deterministic "signature"
  });
  std::string error;
  ASSERT_TRUE(writer.write(1001, {{"dev0000000000000001", 5}}, &error)) << error;
  ASSERT_TRUE(writer.write(2002, {{"dev0000000000000001", 9}}, &error)) << error;
  ASSERT_TRUE(writer.write(3003, {{"dev0000000000000001", 13}}, &error)) << error;
  ASSERT_TRUE(writer.write(4004, {{"dev0000000000000001", 17}}, &error)) << error;

  std::vector<std::string> snapshots;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
    if (entry.path().filename().string().rfind("checkpoint-", 0) == 0 &&
        entry.path().extension() == ".db")
      snapshots.push_back(entry.path().filename().string());
  ASSERT_EQ(snapshots.size(), 3u);  // only the last 3 survive
  std::sort(snapshots.begin(), snapshots.end());
  EXPECT_EQ(snapshots[0], "checkpoint-2002.db");
  EXPECT_EQ(snapshots[2], "checkpoint-4004.db");

  // The manifest carries the merkle root and the snapshot hash.
  std::string content;
  {
    std::ifstream manifest(dir / "checkpoint-4004.db.manifest");
    content.assign((std::istreambuf_iterator<char>(manifest)),
                   std::istreambuf_iterator<char>());
  }  // close before remove_all (Windows keeps open files locked)
  auto json = nlohmann::json::parse(content);
  EXPECT_EQ(json["merkle_root"].get<std::string>().size(), 64u);
  EXPECT_EQ(json["vector"]["dev0000000000000001"].get<int>(), 17);
  EXPECT_FALSE(json["signature"].get<std::string>().empty());
  std::filesystem::remove_all(dir);
}

TEST(CheckpointTest, CompactsOldAuditRowsIntoSummaryWithoutChangingTotals) {
  // M8: a checkpoint retains audit totals but replaces raw ATTEMPT and
  // CRITIQUE rows older than 30 days in its *snapshot*.  The live SQLite
  // view stays replayable from the signed log; the compacted checkpoint is
  // what a storage-constrained joiner can retain.
  const auto dir = std::filesystem::temp_directory_path() /
                   ("onion_compaction_test_" +
                    std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count()));
  std::filesystem::create_directories(dir);
  std::string error;
  {
    Db db(dir / "onion.db");
    ASSERT_TRUE(db.open(&error)) << error;
    const std::int64_t fresh_hlc =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count()
        << 16;
    ASSERT_TRUE(db.exec(
        "INSERT INTO attempts(path,n,score,passed,tokens_in,tokens_cached,"
        "tokens_out,cost,campaign,hlc,origin) VALUES "
        "('P01',1,'0.8',1,100,20,30,'0.0012','c1',1,'dev'),"
        "('P02',1,'0.7',1,40,0,10,'0.0004','c1'," +
            std::to_string(fresh_hlc) + ",'dev');",
        &error)) << error;
    ASSERT_TRUE(db.exec(
        "INSERT INTO critiques(path,score,issues,remove,add_,hlc,origin) VALUES "
        "('P01','0.8','','','',2,'dev'),"
        "('P02','0.7','','',''," + std::to_string(fresh_hlc) + ",'dev');",
        &error)) << error;
  }

  onion::store::CheckpointWriter writer(dir, [](std::string_view bytes) {
    return onion::log::blake2b_hex(bytes);
  });
  ASSERT_TRUE(writer.write(5005, {{"dev", 4}}, &error)) << error;

  Db snapshot(dir / "checkpoint-5005.db");
  ASSERT_TRUE(snapshot.open(&error)) << error;
  auto count = [&](std::string_view table) {
    auto statement = snapshot.prepare("SELECT COUNT(*) FROM " + std::string(table),
                                      &error);
    EXPECT_TRUE(statement.has_value()) << error;
    EXPECT_TRUE(statement->step_row(&error)) << error;
    return statement->integer_int(0);
  };
  EXPECT_EQ(count("attempts"), 1);   // only the fresh P02 row remains raw
  EXPECT_EQ(count("critiques"), 1);  // only the fresh P02 row remains raw

  auto summary = snapshot.prepare(
      "SELECT attempts, critiques, passed, tokens_in, tokens_cached, "
      "tokens_out, cost_nanos FROM checkpoint_summaries WHERE path='P01';",
      &error);
  ASSERT_TRUE(summary.has_value()) << error;
  ASSERT_TRUE(summary->step_row(&error)) << error;
  EXPECT_EQ(summary->integer_int(0), 1);
  EXPECT_EQ(summary->integer_int(1), 1);
  EXPECT_EQ(summary->integer_int(2), 1);
  EXPECT_EQ(summary->integer(3), 100);
  EXPECT_EQ(summary->integer(4), 20);
  EXPECT_EQ(summary->integer(5), 30);
  EXPECT_EQ(summary->integer(6), 1200000);  // exact USD nanounits
  EXPECT_EQ(snapshot.spend_usd(&error), "0.0016");

  const auto manifest = nlohmann::json::parse(
      std::ifstream(dir / "checkpoint-5005.db.manifest"));
  EXPECT_EQ(manifest["compacted_attempts"], 1);
  EXPECT_EQ(manifest["compacted_critiques"], 1);
  std::filesystem::remove_all(dir);
}
