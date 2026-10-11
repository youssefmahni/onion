#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <memory>
#include <string>

#include "sync/sync.hpp"
#include <httplib.h>

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
  // Serve a tampered batch directly from a stub peer (Windows keeps open
  // files locked, so corrupting the live writer's segment is not portable):
  // the client must quarantine it and apply nothing.
  writer_a_->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01-T01"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer_a_->sync();

  // The tampered batch: B's real first batch is valid, but we serve a
  // broken line (bad hash) for the vector's origin.
  auto bad = writer_a_->segments().active_path();
  httplib::Server stub;
  stub.Get("/sync/v1/vector", [&](const httplib::Request&,
                                  httplib::Response& response) {
    nlohmann::json json;
    json["device_id"] = key_a_->device_id;
    json["vector"] = {{key_a_->device_id, 1}};
    response.set_content(json.dump(), "application/json");
  });
  stub.Get("/sync/v1/log/(.*)", [&](const httplib::Request&,
                                    httplib::Response& response) {
    response.set_content(
        "{\"v\":1,\"o\":\"" + key_a_->device_id +
            "\",\"b\":1,\"prev\":\"00\",\"hash\":\"00\","
            "\"sig\":\"00\",\"recs\":[{\"s\":1,\"h\":1,"
            "\"t\":\"SETTLE\",\"p\":{\"path\":\"P01-IN01-T01\","
            "\"kept\":0,\"end\":\"cannot split\"}}]}\n",
        "application/x-ndjson");
  });
  const int stub_port = stub.bind_to_any_port("127.0.0.1");
  std::atomic<bool> stub_running{true};
  std::thread stub_thread([&] {
    while (stub_running.load())
      if (!stub.listen_after_bind()) break;
  });
  (void)bad;

  SyncClient client = make_client_b();
  std::map<std::string, std::string> quarantined;
  std::string error;
  std::fprintf(stderr, "test: running round\n");
  const int applied = client.run_round(
      "127.0.0.1:" + std::to_string(stub_port), &quarantined, &error);
  std::fprintf(stderr, "test: round done applied=%d\n", applied);
  stub_running.store(false);
  stub.stop();
  std::fprintf(stderr, "test: stub stopped\n");
  stub_thread.join();
  std::fprintf(stderr, "test: thread joined\n");
  EXPECT_EQ(applied, 0);
  EXPECT_FALSE(quarantined.empty());
  EXPECT_FALSE(writer_b_->settled("P01-IN01-T01"));
}

}  // namespace

TEST_F(SyncTest, RepeatedRoundsAreIdempotent) {
  writer_a_->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01-T01"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer_a_->sync();
  start_server_a();
  SyncClient client = make_client_b();
  const std::string peer = "127.0.0.1:" + std::to_string(server_->bound_port());

  std::map<std::string, std::string> quarantined;
  std::string error;
  const int first = client.run_round(peer, &quarantined, &error);
  EXPECT_GE(first, 1);
  EXPECT_TRUE(quarantined.empty());

  // A duplicate delivery (the same round again) applies nothing new: the
  // vector is ahead, so there is nothing to pull, and nothing double-folds.
  quarantined.clear();
  const int second = client.run_round(peer, &quarantined, &error);
  EXPECT_EQ(second, 0);
  EXPECT_TRUE(quarantined.empty());

  // A dropped connection retried later also converges: the vector already
  // matches, so a third round changes nothing.
  const int third = client.run_round(peer, &quarantined, &error);
  EXPECT_EQ(third, 0);
  EXPECT_TRUE(writer_b_->settled("P01-IN01-T01"));
  std::string query_error;
  EXPECT_EQ(writer_b_->db().settles(&query_error).size(), 1u);
}

TEST_F(SyncTest, MidChainPullContinuesTheLocalChainHead) {
  // A writes two batches. B first holds only batch 1 (as if synced in an
  // earlier round), then a sync round must continue from that chain head
  // and pull batch 2 — the liveness property fixed in M6.
  writer_a_->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01-T01"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer_a_->sync();
  writer_a_->submit(CommitTerminal{{
      make_record("SETTLE", {{"path", "P01-IN01-T02"}, {"kept", 0},
                             {"end", "cannot split"}}),
  }});
  writer_a_->sync();

  // Hand B the first batch only, as the previous round did.
  {
    std::ifstream in(writer_a_->segments().active_path());
    std::string line;
    std::getline(in, line);
    auto batch_json = nlohmann::json::parse(line);
    onion::log::Batch batch;
    batch.version = batch_json.value("v", 0);
    batch.origin = batch_json.value("o", std::string{});
    batch.number = batch_json.value("b", std::uint64_t{0});
    batch.prev = batch_json.value("prev", std::string{});
    batch.hash = batch_json.value("hash", std::string{});
    batch.sig = batch_json.value("sig", std::string{});
    for (const auto& record_json : batch_json["recs"])
      batch.records.push_back(onion::log::Record::from_json(record_json));
    // The precondition: the first line of A's active segment is batch 1 of
    // A (no auto genesis batch, no sealing at default sizes).
    EXPECT_EQ(batch.number, 1u);
    EXPECT_EQ(batch.origin, key_a_->device_id);
    writer_b_->submit(onion::state::AppendForeign{std::move(batch)});
    writer_b_->sync();
  }
  EXPECT_TRUE(writer_b_->settled("P01-IN01-T01"));
  EXPECT_FALSE(writer_b_->settled("P01-IN01-T02"));

  start_server_a();
  SyncClient client = make_client_b();
  std::map<std::string, std::string> quarantined;
  std::string error;
  const int applied = client.run_round(
      "127.0.0.1:" + std::to_string(server_->bound_port()), &quarantined,
      &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_TRUE(quarantined.empty());
  EXPECT_EQ(applied, 1);  // the second batch
  EXPECT_TRUE(writer_b_->settled("P01-IN01-T02"));
}
