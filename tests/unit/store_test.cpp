#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>

#include "store/store.hpp"

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

}  // namespace
