#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>

#include "log/log.hpp"

namespace {

using onion::log::Batch;
using onion::log::HlcClock;
using onion::log::NullSigner;
using onion::log::Record;
using onion::log::Segments;
using onion::log::blake2b_hex;
using onion::log::canonical_json;
using onion::log::hash_records_hex;
using onion::log::make_batch;
using onion::log::records_json;
using nlohmann::json;

Record record(std::int64_t seq, onion::log::Hlc hlc, std::string type,
              json payload) {
  Record r;
  r.seq = seq;
  r.hlc = hlc;
  r.type = std::move(type);
  r.payload = std::move(payload);
  return r;
}

TEST(CanonicalJson, SortsKeysAndOmitsWhitespace) {
  const json value{{"b", 2}, {"a", 1}, {"c", "x"}};
  EXPECT_EQ(canonical_json(value), R"({"a":1,"b":2,"c":"x"})");
}

TEST(CanonicalJson, IntegersAreExactDecimal) {
  // A current HLC exceeds 2^53 and must keep every bit (decision 0003).
  const json value{{"h", static_cast<std::int64_t>(115344015360000001LL)},
                   {"neg", static_cast<std::int64_t>(-9223372036854775807LL)}};
  EXPECT_EQ(canonical_json(value),
            R"({"h":115344015360000001,"neg":-9223372036854775807})");
  const json unsigned_value{{"u", std::numeric_limits<std::uint64_t>::max()}};
  EXPECT_EQ(canonical_json(unsigned_value), R"({"u":18446744073709551615})");
}

TEST(CanonicalJson, EscapesLikeRfc8785) {
  const json value(std::string("quote\" backslash\\ newline\n tab\t ctrl\x01"));
  EXPECT_EQ(canonical_json(value), R"("quote\" backslash\\ newline\n tab\t ctrl\u0001")");
}

TEST(CanonicalJson, RejectsFloats) {
  EXPECT_THROW(canonical_json(json{{"f", 0.85}}), std::domain_error);
}

TEST(CanonicalJson, NestedArraysAndObjects) {
  const json value{{"recs", json::array({json{{"s", 1}, {"t", "SETTLE"}}})}};
  EXPECT_EQ(canonical_json(value), R"({"recs":[{"s":1,"t":"SETTLE"}]})");
}

TEST(Records, CanonicalBytesAreStable) {
  const std::vector<Record> records = {
      record(1, 0x1234, "ADD_NODE",
             json{{"path", "P01-IN01"}, {"layer", 1}, {"title", "In"}}),
      record(2, 0x1235, "SETTLE",
             json{{"path", "P01-IN01"}, {"kept", 3}, {"end", "children"}})};
  const std::string canonical = records_json(records);
  EXPECT_EQ(canonical, R"([{"h":4660,"p":{"layer":1,"path":"P01-IN01","title":"In"},"s":1,"t":"ADD_NODE"},{"h":4661,"p":{"end":"children","kept":3,"path":"P01-IN01"},"s":2,"t":"SETTLE"}])");
  // Identical input, identical bytes: the hash is deterministic.
  EXPECT_EQ(hash_records_hex(records), blake2b_hex(canonical));
}

TEST(MakeBatch, NullSignerHashesAndChains) {
  const std::vector<Record> records = {
      record(1, 0x1, "ADD_NODE", json{{"path", "P01"}, {"layer", 0}})};
  NullSigner signer;
  const Batch first = make_batch("9f2c41ab07de5531", 1, "", records, signer);
  EXPECT_EQ(first.hash, hash_records_hex(records));
  EXPECT_TRUE(first.sig.empty());
  EXPECT_EQ(first.prev, "");  // batch 1 uses the public-key hash (M4); M3 empty
  EXPECT_EQ(first.number, 1u);

  const Batch second =
      make_batch("9f2c41ab07de5531", 2, first.hash, records, signer);
  EXPECT_EQ(second.prev, first.hash);
  EXPECT_EQ(second.hash, first.hash);
  EXPECT_EQ(second.number, 2u);
}

TEST(HlcClock, IsStrictlyMonotonic) {
  HlcClock clock;
  onion::log::Hlc previous = clock.now();
  for (int i = 0; i < 1000; ++i) {
    const onion::log::Hlc next = clock.now();
    EXPECT_GT(next, previous);
    previous = next;
  }
  // The physical part is the wall-clock millisecond count.
  const std::uint64_t wall_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  EXPECT_NEAR(static_cast<double>(previous >> 16), static_cast<double>(wall_ms),
              2000.0);
}

class SegmentsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("onion_log_test_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  Segments open_segments(std::uint64_t seal_bytes = 64u * 1024u * 1024u) {
    Segments::Options options;
    options.dir = dir_ / "oplog" / "dev0000000000000001";
    options.origin = "dev0000000000000001";
    options.seal_bytes = seal_bytes;
    Segments segments(options);
    std::string error;
    EXPECT_TRUE(segments.open(&error)) << error;
    return segments;
  }

  Batch batch_with(std::uint64_t number, std::int64_t first_seq,
                   std::string prev) {
    const std::vector<Record> records = {
        record(first_seq, first_seq, "ADD_NODE",
               json{{"path", "P01-TK00001"}, {"layer", 1}}),
        record(first_seq + 1, first_seq + 1, "SETTLE",
               json{{"path", "P01-TK00001"}, {"kept", 0}, {"end", "cannot split"}})};
    NullSigner signer;
    return make_batch("dev0000000000000001", number, std::move(prev), records,
                      signer);
  }

  std::filesystem::path dir_;
};

TEST_F(SegmentsTest, AppendsOneLinePerBatchAndReopens) {
  {
    Segments segments = open_segments();
    std::string error;
    EXPECT_TRUE(segments.append(batch_with(1, 1, ""), &error)) << error;
    EXPECT_TRUE(segments.append(batch_with(2, 3, "0000000000000000000000000000000000000000000000000000000000000000"), &error))
        << error;
    EXPECT_EQ(segments.total_batches(), 2u);
    EXPECT_EQ(segments.active_first_seq(), 1u);
  }
  {
    Segments reopened = open_segments();
    EXPECT_EQ(reopened.total_batches(), 2u);
    EXPECT_EQ(reopened.active_first_seq(), 1u);
    std::string error;
    EXPECT_TRUE(reopened.append(batch_with(3, 5, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"), &error))
        << error;
    EXPECT_EQ(reopened.total_batches(), 3u);
  }
}

TEST_F(SegmentsTest, CutsTornTailWithoutFinalLf) {
  {
    Segments segments = open_segments();
    std::string error;
    EXPECT_TRUE(segments.append(batch_with(1, 1, ""), &error)) << error;
  }
  // Simulate a crash mid-write: one trailing batch without its final LF.
  {
    std::ofstream out(open_segments().active_path(), std::ios::app);
    out << R"({"v":1,"o":"dev0000000000000001","b":2,"prev":"00","hash":"00","sig":"","recs":[{"s":3,"h":3,"t":"ADD_NODE","p":{"path":"P02"}})";
  }
  Segments repaired = open_segments();
  EXPECT_EQ(repaired.total_batches(), 1u);
  std::string error;
  EXPECT_TRUE(repaired.append(batch_with(2, 3, "11"), &error)) << error;
  EXPECT_EQ(repaired.total_batches(), 2u);
}

TEST_F(SegmentsTest, RotatesWhenSealBytesWouldBeCrossed) {
  Segments segments = open_segments(512);
  std::string error;
  EXPECT_TRUE(segments.append(batch_with(1, 1, ""), &error)) << error;
  EXPECT_TRUE(segments.append(batch_with(2, 3, "11"), &error)) << error;
  EXPECT_TRUE(segments.append(batch_with(3, 5, "22"), &error)) << error;
  EXPECT_EQ(segments.total_batches(), 1u);  // only the newest segment's count
  EXPECT_EQ(segments.active_first_seq(), 5u);
  const std::filesystem::path expected = dir_ / "oplog" / "dev0000000000000001" / "5.log";
  EXPECT_EQ(segments.active_path(), expected);
  EXPECT_TRUE(std::filesystem::exists(expected));
}

}  // namespace
