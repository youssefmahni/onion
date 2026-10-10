#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>

#include "sync/sync.hpp"

namespace {

using onion::log::DeviceKey;
using onion::log::Ed25519Signer;
using onion::state::CommitTerminal;
using onion::state::StateWriter;
using onion::sync::MemberKey;
using onion::sync::NonceCache;
using onion::sync::SyncClient;
using onion::sync::SyncServer;
using onion::sync::request_signature_valid;
using onion::sync::sign_request;
using nlohmann::json;

std::vector<std::uint8_t> seed(int base) {
  std::vector<std::uint8_t> bytes(32);
  for (std::size_t i = 0; i < 32; ++i)
    bytes[i] = static_cast<std::uint8_t>(base + i);
  return bytes;
}

onion::log::Record make_record(std::string type, nlohmann::json payload) {
  onion::log::Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  return record;
}

std::unique_ptr<StateWriter> open_writer(const std::filesystem::path& dir,
                                         const DeviceKey& key) {
  StateWriter::Options options;
  options.data_dir = dir;
  options.origin = key.device_id;
  options.device_key = std::make_shared<DeviceKey>(key);
  options.signer = std::make_unique<Ed25519Signer>(key);
  auto writer = std::make_unique<StateWriter>(std::move(options));
  std::string error;
  EXPECT_TRUE(writer->start(&error)) << error;
  return writer;
}

class SyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("onion_sync_test_" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()));
    key_a_ = std::make_shared<DeviceKey>(DeviceKey::from_seed(seed(0)));
    key_b_ = std::make_shared<DeviceKey>(DeviceKey::from_seed(seed(16)));
    writer_a_ = open_writer(root_ / "a", *key_a_);
    writer_b_ = open_writer(root_ / "b", *key_b_);
  }
  void TearDown() override {
    if (server_) server_->stop();
    if (writer_a_) writer_a_->stop();
    if (writer_b_) writer_b_->stop();
    std::filesystem::remove_all(root_);
  }

  void start_server_a() {
    SyncServer::Options options;
    options.port = 0;
    options.bind_address = "127.0.0.1";
    options.writer = writer_a_.get();
    options.key = key_a_;
    options.members = {{key_a_->device_id, key_a_->public_key},
                       {key_b_->device_id, key_b_->public_key}};
    server_ = std::make_unique<SyncServer>(std::move(options));
    std::string error;
    ASSERT_TRUE(server_->start(&error)) << error;
    std::fprintf(stderr, "sync server on port %d\n", server_->bound_port());
  }

  SyncClient make_client_b() {
    SyncClient::Options options{
        *writer_b_,
        key_b_,
        {{key_a_->device_id, {key_a_->device_id, key_a_->public_key}},
         {key_b_->device_id, {key_b_->device_id, key_b_->public_key}}}};
    return SyncClient(std::move(options));
  }

  std::filesystem::path root_;
  std::shared_ptr<DeviceKey> key_a_;
  std::shared_ptr<DeviceKey> key_b_;
  std::unique_ptr<StateWriter> writer_a_;
  std::unique_ptr<StateWriter> writer_b_;
  std::unique_ptr<SyncServer> server_;
};

TEST_F(SyncTest, RequestSignatureRoundTripAndRejections) {
  Ed25519Signer signer(*key_a_);
  const std::string signature = sign_request(key_a_->device_id, "GET",
                                             "/sync/v1/vector", "body", 1000,
                                             signer);
  EXPECT_TRUE(request_signature_valid(key_a_->device_id, "GET",
                                      "/sync/v1/vector", "body", signature,
                                      key_a_->public_key, 1000, 1000));
  // Stale timestamp.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "body", signature,
                                       key_a_->public_key, 1000, 2000));
  // Wrong public key.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "body", signature,
                                       key_b_->public_key, 1000, 1000));
  // Tampered body.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "tampered",
                                       signature, key_a_->public_key, 1000,
                                       1000));
}

TEST_F(SyncTest, NonceCacheRejectsReplaysInTheWindow) {
  NonceCache cache;
  EXPECT_FALSE(cache.seen_and_add("dev1", "sig1", 1000));
  EXPECT_TRUE(cache.seen_and_add("dev1", "sig1", 1000));    // replay
  EXPECT_TRUE(cache.seen_and_add("dev1", "sig1", 1050));    // same window
  EXPECT_FALSE(cache.seen_and_add("dev1", "sig2", 1050));   // new signature
  EXPECT_FALSE(cache.seen_and_add("dev2", "sig1", 1050));   // other device
  EXPECT_FALSE(cache.seen_and_add("dev1", "sig1", 2000));   // window moved on
}

TEST_F(SyncTest, OneRoundConvergesBothSettlesAndNodes) {
  // A commits two batches.
  writer_a_->submit(CommitTerminal{{
      make_record("ADD_NODE", {{"path", "P01-IN01-T01-TK00001"},
                               {"layer", 4},
                               {"title", "From A"}}),
      make_record("SETTLE", {{"path", "P01-IN01-T01"}, {"kept", 1},
                             {"end", "children"}}),
  }});
  writer_a_->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01-T02"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer_a_->sync();

  start_server_a();
  SyncClient client = make_client_b();
  std::map<std::string, std::string> quarantined;
  std::string error;
  const int applied = client.run_round(
      "127.0.0.1:" + std::to_string(server_->bound_port()), &quarantined,
      &error);
  ASSERT_TRUE(error.empty()) << error;
  if (applied < 2) {
    for (const auto& [origin, reason] : quarantined)
      std::fprintf(stderr, "quarantined %s: %s\n", origin.c_str(), reason.c_str());
  }
  EXPECT_GE(applied, 2);
  EXPECT_TRUE(quarantined.empty());

  // B now agrees with A: both settles folded, the node exists, and the
  // vector matches.
  EXPECT_TRUE(writer_b_->settled("P01-IN01-T01"));
  EXPECT_TRUE(writer_b_->settled("P01-IN01-T02"));
  std::string query_error;
  const auto node = writer_b_->db().node_by_path("P01-IN01-T01-TK00001", &query_error);
  ASSERT_TRUE(node.has_value()) << query_error;
  EXPECT_EQ(node->title, "From A");
  EXPECT_EQ(writer_b_->vector().at(key_a_->device_id),
            writer_a_->vector().at(key_a_->device_id));
}

TEST_F(SyncTest, VerificationFailureQuarantinesAndDoesNotApply) {
  writer_a_->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01-T01"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer_a_->sync();

  // Corrupt the last byte of the batch line in A's segment: the hash check
  // must quarantine it on B.
  {
    const auto path = writer_a_->segments().active_path();
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();
    content[content.size() - 2] = '0';
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
  }

  start_server_a();
  SyncClient client = make_client_b();
  std::map<std::string, std::string> quarantined;
  std::string error;
  const int applied = client.run_round(
      "127.0.0.1:" + std::to_string(server_->bound_port()), &quarantined,
      &error);
  EXPECT_EQ(applied, 0);
  EXPECT_FALSE(quarantined.empty());
  EXPECT_FALSE(writer_b_->settled("P01-IN01-T01"));
}

}  // namespace
