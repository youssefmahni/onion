#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>

#include "log/log.hpp"
#include "log/invite.hpp"

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

// ------------------------------------------------------ M4: keys and verify

#include <fstream>

namespace {
std::vector<std::uint8_t> unhex_vec(std::string_view hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    bytes.push_back(static_cast<std::uint8_t>((digit(hex[i]) << 4) | digit(hex[i + 1])));
  }
  return bytes;
}
}  // namespace

TEST(DeviceKey, DeterministicIdentityFromSeed) {
  using onion::log::DeviceKey;
  const std::vector<std::uint8_t> seed =
      unhex_vec("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const DeviceKey key = DeviceKey::from_seed(seed);
  EXPECT_EQ(key.seed_hex(),
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  EXPECT_EQ(key.device_id.size(), 16u);
  EXPECT_GE(key.slot, 0);
  EXPECT_LT(key.slot, 1 << 20);
  const DeviceKey again = DeviceKey::from_seed(seed);
  EXPECT_EQ(again.device_id, key.device_id);
  EXPECT_EQ(again.slot, key.slot);
  EXPECT_EQ(onion::log::genesis_prev(again), onion::log::genesis_prev(key));
}

TEST(Ed25519Signer, MatchesMonocypherGoldenSignature) {
  // The M1 golden project batch was signed from seed 00..1f over the
  // canonical records bytes; the M4 signer over the same bytes must agree.
  const std::vector<std::uint8_t> seed =
      unhex_vec("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  onion::log::DeviceKey key = onion::log::DeviceKey::from_seed(seed);
  onion::log::Ed25519Signer signer(key);
  const std::string message =
      "[{\"h\":115344015360000001,\"p\":{\"layer\":-7,\"title\":\"Café العربية "
      "\\\"quote\\\" \\\\ slash\\n\"},\"s\":1,\"t\":\"ADD_NODE\"},"
      "{\"h\":115344015360000002,\"p\":{\"end\":\"children\",\"kept\":1},"
      "\"s\":2,\"t\":\"SETTLE\"}]";
  const std::string signature = signer.sign(message);
  std::ifstream fixture_file(std::filesystem::path(ONION_GOLDEN_DIR) /
                             "project-batch-signature.hex");
  std::string expected((std::istreambuf_iterator<char>(fixture_file)),
                       std::istreambuf_iterator<char>());
  while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r'))
    expected.pop_back();
  EXPECT_EQ(signature, expected);
}

namespace {
onion::log::Batch signed_batch(std::uint64_t number, std::string prev,
                               std::vector<onion::log::Record> records,
                               onion::log::Signer& signer) {
  return onion::log::make_batch("9f2c41ab07de5531", number, std::move(prev),
                                std::move(records), signer);
}
}  // namespace

class VerifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::vector<std::uint8_t> seed =
        unhex_vec("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    key_ = onion::log::DeviceKey::from_seed(seed);
    signer_ = std::make_unique<onion::log::Ed25519Signer>(key_);
    first_ = signed_batch(1, onion::log::genesis_prev(key_),
                          {record(1, 1, "ADD_NODE", json{{"path", "P01"}, {"layer", 0}})},
                          *signer_);
    context_.version = 1;
    context_.expected_prev = onion::log::genesis_prev(key_);
    context_.expected_first_seq = 1;
    context_.members = &members_;
    context_.origin_public_key = &key_.public_key;
  }

  onion::log::VerifyContext context_;
  std::vector<std::string> members_{"9f2c41ab07de5531"};
  onion::log::DeviceKey key_;
  std::unique_ptr<onion::log::Ed25519Signer> signer_;
  onion::log::Batch first_;
};

TEST_F(VerifyTest, RoundTripIsOk) {
  EXPECT_EQ(onion::log::verify(first_, context_), onion::log::Verify::Ok);
}

TEST_F(VerifyTest, TamperedSignatureIsBadSig) {
  onion::log::Batch tampered = first_;
  tampered.sig[0] = tampered.sig[0] == '0' ? '1' : '0';
  EXPECT_EQ(onion::log::verify(tampered, context_), onion::log::Verify::BadSig);
}

TEST_F(VerifyTest, WrongPrevIsBadPrev) {
  onion::log::VerifyContext wrong_prev = context_;
  wrong_prev.expected_prev = std::string(64, 'f');
  EXPECT_EQ(onion::log::verify(first_, wrong_prev), onion::log::Verify::BadPrev);
}

TEST_F(VerifyTest, TamperedRecordIsBadHash) {
  onion::log::Batch tampered = first_;
  tampered.records[0].payload["title"] = "tampered";
  EXPECT_EQ(onion::log::verify(tampered, context_), onion::log::Verify::BadHash);
}

TEST_F(VerifyTest, GapInRecordNumbersIsGap) {
  onion::log::VerifyContext gap = context_;
  gap.expected_first_seq = 2;
  EXPECT_EQ(onion::log::verify(first_, gap), onion::log::Verify::Gap);
}

TEST_F(VerifyTest, UnknownVersionIsBadVersion) {
  onion::log::Batch tampered = first_;
  tampered.version = 2;
  EXPECT_EQ(onion::log::verify(tampered, context_), onion::log::Verify::BadVersion);
}

TEST_F(VerifyTest, UnknownOriginIsNotMember) {
  onion::log::Batch foreign = first_;
  foreign.origin = "deadbeefdeadbeef";
  EXPECT_EQ(onion::log::verify(foreign, context_), onion::log::Verify::NotMember);
}

TEST_F(VerifyTest, AboveCutoffIsPastCutoff) {
  onion::log::VerifyContext cutoff = context_;
  cutoff.evict_cutoff = 0;
  EXPECT_EQ(onion::log::verify(first_, cutoff), onion::log::Verify::Ok);
  cutoff.evict_cutoff = -1;
  EXPECT_EQ(onion::log::verify(first_, cutoff), onion::log::Verify::PastCutoff);
}

TEST_F(VerifyTest, ChainAcrossBatches) {
  onion::log::Batch second = signed_batch(
      2, first_.hash,
      {record(2, 2, "SETTLE", json{{"path", "P01"}, {"kept", 0}, {"end", "cannot split"}})},
      *signer_);
  onion::log::VerifyContext second_context = context_;
  second_context.expected_prev = first_.hash;
  second_context.expected_first_seq = 2;
  EXPECT_EQ(onion::log::verify(second, second_context), onion::log::Verify::Ok);
}

// ------------------------------------------------------------- evictions

TEST(EvictionValid, LoneMemberSignatureIsRejected) {
  const std::vector<std::uint8_t> seed_a = unhex_vec(
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const std::vector<std::uint8_t> seed_b = unhex_vec(
      "101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f");
  const std::vector<std::uint8_t> seed_admin = unhex_vec(
      "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f");
  const auto key_a = onion::log::DeviceKey::from_seed(seed_a);
  const auto key_b = onion::log::DeviceKey::from_seed(seed_b);
  const auto key_admin = onion::log::DeviceKey::from_seed(seed_admin);
  const auto pub = [](const onion::log::DeviceKey& key) {
    std::string hex;
    for (const std::uint8_t byte : key.public_key) {
      hex += "0123456789abcdef"[byte >> 4];
      hex += "0123456789abcdef"[byte & 0x0f];
    }
    return hex;
  };
  const std::vector<std::string> members = {pub(key_a), pub(key_b)};

  // One member signing alone: rejected (needs a majority of 2, or admin).
  onion::log::Record record;
  record.type = "MEMBER_EVICT";
  record.payload = {{"device_id", "deadbeefdeadbeef"},
                    {"accept_until_seq", 10},
                    {"sigs", json::array()},
                    {"kind", "majority"}};
  const nlohmann::json unsigned_payload = record.payload;
  nlohmann::json cleaned = unsigned_payload;
  cleaned.erase("sigs");
  const std::string bytes = onion::log::canonical_json(cleaned);
  onion::log::Ed25519Signer signer_a(key_a);
  record.payload["sigs"] = json::array({signer_a.sign(bytes)});
  EXPECT_FALSE(onion::log::eviction_valid(record, members, pub(key_admin)));

  // Both members: a majority.
  onion::log::Ed25519Signer signer_b(key_b);
  record.payload["sigs"] = json::array({signer_a.sign(bytes), signer_b.sign(bytes)});
  EXPECT_TRUE(onion::log::eviction_valid(record, members, pub(key_admin)));

  // A single admin signature: accepted. The signed bytes carry kind=admin.
  record.payload["kind"] = "admin";
  record.payload["sigs"] = json::array();
  const nlohmann::json admin_cleaned = record.payload;
  nlohmann::json admin_without_sigs = admin_cleaned;
  admin_without_sigs.erase("sigs");
  const std::string admin_bytes = onion::log::canonical_json(admin_without_sigs);
  onion::log::Ed25519Signer signer_admin(key_admin);
  record.payload["sigs"] = json::array({signer_admin.sign(admin_bytes)});
  EXPECT_TRUE(onion::log::eviction_valid(record, members, pub(key_admin)));

  // A non-admin single signature with kind=admin: rejected.
  record.payload["sigs"] = json::array({signer_a.sign(admin_bytes)});
  EXPECT_FALSE(onion::log::eviction_valid(record, members, pub(key_admin)));
}

namespace {
TEST(InviteToken, RoundTrip) {
  const auto inviter = onion::log::DeviceKey::from_seed_hex(
      "000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f");
  onion::log::Ed25519Signer signer(inviter);
  const auto invitee = onion::log::DeviceKey::from_seed_hex(
      "202122232425262728292a2b2c2d2e2f"
      "303132333435363738393a3b3c3d3e3f");
  std::string pubkey_hex;
  for (const std::uint8_t byte : invitee.public_key) {
    pubkey_hex += "0123456789abcdef"[byte >> 4];
    pubkey_hex += "0123456789abcdef"[byte & 0x0f];
  }

  const std::int64_t now = 1'700'000'000;
  const auto token = onion::log::issue_invite(
      pubkey_hex, inviter.device_id, 3600, "0f1e2d3c4b5a69788796a5b4c3d2e1f0",
      now, signer);
  EXPECT_EQ(token.expires_at_s, now + 3600);

  std::string error;
  const auto parsed = onion::log::parse_invite(
      onion::log::invite_to_string(token), now + 100, inviter.public_key,
      &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->pubkey_hex, pubkey_hex);
  EXPECT_EQ(parsed->nonce, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");

  EXPECT_FALSE(onion::log::parse_invite(onion::log::invite_to_string(token),
                                        now + 3601, inviter.public_key,
                                        &error)
                   .has_value());
  EXPECT_FALSE(onion::log::parse_invite(onion::log::invite_to_string(token),
                                        now + 100, invitee.public_key,
                                        &error)
                   .has_value());
  nlohmann::json tampered =
      nlohmann::json::parse(onion::log::invite_to_string(token));
  tampered["nonce"] = "deadbeefdeadbeefdeadbeefdeadbeef";
  EXPECT_FALSE(onion::log::parse_invite(tampered.dump(), now + 100,
                                        inviter.public_key, &error)
                   .has_value());
  EXPECT_FALSE(onion::log::parse_invite("{nope", now, inviter.public_key,
                                        &error)
                   .has_value());
  // Expiry at exactly expires_at_s rejects (the inviter checks the token
  // on its own clock; no cross-machine grace window).
  EXPECT_FALSE(onion::log::parse_invite(onion::log::invite_to_string(token),
                                        now + 3600, inviter.public_key,
                                        &error)
                   .has_value());
  // A token with extra fields still verifies: the signature covers the
  // rebuilt canonical payload, not the received bytes.
  nlohmann::json with_extras =
      nlohmann::json::parse(onion::log::invite_to_string(token));
  with_extras["comment"] = "ignored";
  EXPECT_TRUE(onion::log::parse_invite(with_extras.dump(), now + 100,
                                       inviter.public_key, &error)
                  .has_value());
  // Odd-length nonce: two distinct strings must never decode identically.
  nlohmann::json odd = nlohmann::json::parse(onion::log::invite_to_string(token));
  odd["nonce"] = "abc";  // 1.5 bytes of hex
  EXPECT_FALSE(onion::log::parse_invite(odd.dump(), now + 100,
                                        inviter.public_key, &error)
                   .has_value());
}

TEST(InviteToken, TtlParsesShorthands) {
  std::string error;
  EXPECT_EQ(onion::log::parse_ttl("24h", &error), 86400);
  EXPECT_EQ(onion::log::parse_ttl("30m", &error), 1800);
  EXPECT_EQ(onion::log::parse_ttl("1d", &error), 86400);
  EXPECT_EQ(onion::log::parse_ttl("90", &error), 90);
  EXPECT_FALSE(onion::log::parse_ttl("", &error).has_value());
  EXPECT_FALSE(onion::log::parse_ttl("h", &error).has_value());
  EXPECT_FALSE(onion::log::parse_ttl("-5h", &error).has_value());
  EXPECT_FALSE(onion::log::parse_ttl("0", &error).has_value());
  EXPECT_FALSE(onion::log::parse_ttl("9999999999999h", &error).has_value());
  EXPECT_FALSE(onion::log::parse_ttl("+5", &error).has_value());
  EXPECT_EQ(onion::log::parse_ttl(" 5 ", &error), 5);
}
}  // namespace
