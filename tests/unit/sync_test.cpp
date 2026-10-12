#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <memory>
#include <string>

#include "sync/sync.hpp"
#include <httplib.h>

#include "log/invite.hpp"
#include "store/checkpoint.hpp"

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
  const std::string nonce = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
  const std::string signature = sign_request(
      key_a_->device_id, "GET", "/sync/v1/vector", "body", nonce, 1000,
      signer);
  EXPECT_TRUE(request_signature_valid(key_a_->device_id, "GET",
                                      "/sync/v1/vector", "body", nonce,
                                      signature, key_a_->public_key, 1000,
                                      1000));
  // The nonce is part of the signed bytes: any change breaks the check.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "body", "deadbeef",
                                       signature, key_a_->public_key, 1000,
                                       1000));
  // Missing nonce.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "body", "",
                                       signature, key_a_->public_key, 1000,
                                       1000));
  // Stale timestamp.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "body", nonce,
                                       signature, key_a_->public_key, 1000,
                                       2000));
  // Wrong public key.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "body", nonce,
                                       signature, key_b_->public_key, 1000,
                                       1000));
  // Tampered body.
  EXPECT_FALSE(request_signature_valid(key_a_->device_id, "GET",
                                       "/sync/v1/vector", "tampered", nonce,
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


TEST_F(SyncTest, JoinEndpointAdmitsAValidToken) {
  // A third device joins through an invitation signed by device B.
  const auto key_c = onion::log::DeviceKey::from_seed_hex(
      "303132333435363738393a3b3c3d3e3f"
      "404142434445464748494a4b4c4d4e4f");
  std::string pubkey_c_hex;
  for (const std::uint8_t byte : key_c.public_key) {
    pubkey_c_hex += "0123456789abcdef"[byte >> 4];
    pubkey_c_hex += "0123456789abcdef"[byte & 0x0f];
  }
  onion::log::Ed25519Signer inviter_signer(*key_b_);
  const std::int64_t now_s = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto token = onion::log::issue_invite(
      pubkey_c_hex, key_b_->device_id, 3600,
      "0f1e2d3c4b5a69788796a5b4c3d2e1f0", now_s, inviter_signer);

  start_server_a();
  httplib::Client client(
      ("http://127.0.0.1:" + std::to_string(server_->bound_port())).c_str());
  const auto response =
      client.Post("/sync/v1/join", onion::log::invite_to_string(token),
                  "application/json");
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->status, 200);
  const auto body = nlohmann::json::parse(response->body, nullptr, false);
  EXPECT_TRUE(body.contains("device_id"));
  EXPECT_EQ(body["device_id"], key_c.device_id);
  // The admission response carries the fleet member list (the joiner
  // builds its members.json from it): the static members plus the new
  // device.
  ASSERT_TRUE(body.contains("members"));
  EXPECT_EQ(body["members"].size(), 3u);  // a, b, and the joined device

  // The admission record folded: the member row exists (JOINING) and the
  // nonce is burned.
  std::string error;
  auto statement = writer_a_->db().prepare(
      "SELECT COUNT(*) FROM members WHERE device_id = ?1;", &error);
  ASSERT_TRUE(statement);
  std::string bind_error;
  statement->bind(1, key_c.device_id, &bind_error);
  std::string step_error;
  ASSERT_TRUE(statement->step_row(&step_error));
  EXPECT_EQ(statement->integer(0), 1);
  EXPECT_TRUE(writer_a_->join_nonce_seen("0f1e2d3c4b5a69788796a5b4c3d2e1f0"));

  // Replay: the single-use nonce refuses a second admission.
  const auto replay = client.Post("/sync/v1/join",
                                  onion::log::invite_to_string(token),
                                  "application/json");
  ASSERT_NE(replay, nullptr);
  EXPECT_EQ(replay->status, 409);
}

TEST_F(SyncTest, JoinEndpointRejectsTamperedAndExpiredTokens) {
  const auto key_c = onion::log::DeviceKey::from_seed_hex(
      "505152535455565758595a5b5c5d5e5f"
      "606162636465666768696a6b6c6d6e6f");
  std::string pubkey_c_hex;
  for (const std::uint8_t byte : key_c.public_key) {
    pubkey_c_hex += "0123456789abcdef"[byte >> 4];
    pubkey_c_hex += "0123456789abcdef"[byte & 0x0f];
  }
  onion::log::Ed25519Signer inviter_signer(*key_b_);
  const std::int64_t now_s = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  start_server_a();
  httplib::Client client(
      ("http://127.0.0.1:" + std::to_string(server_->bound_port())).c_str());

  // Expired.
  const auto expired = onion::log::issue_invite(
      pubkey_c_hex, key_b_->device_id, 3600,
      "1f2e3d4c5b6a7988a9bacbdcedfe0f10", now_s - 7200, inviter_signer);
  const auto expired_response =
      client.Post("/sync/v1/join", onion::log::invite_to_string(expired),
                  "application/json");
  ASSERT_NE(expired_response, nullptr);
  EXPECT_EQ(expired_response->status, 401);

  // Tampered nonce (the signature covers it).
  const auto valid = onion::log::issue_invite(
      pubkey_c_hex, key_b_->device_id, 3600,
      "2f3e4d5c6b7a8998a9bacbdcedfe0f11", now_s, inviter_signer);
  nlohmann::json tampered =
      nlohmann::json::parse(onion::log::invite_to_string(valid));
  tampered["nonce"] = "deadbeefdeadbeefdeadbeefdeadbeef";
  const auto tampered_response = client.Post("/sync/v1/join", tampered.dump(),
                                             "application/json");
  ASSERT_NE(tampered_response, nullptr);
  EXPECT_EQ(tampered_response->status, 401);

  // Unknown inviter (signed by a device that is not a member).
  const auto stranger = onion::log::DeviceKey::from_seed_hex(
      "707172737475767778797a7b7c7d7e7f"
      "808182838485868788898a8b8c8d8e8f");
  onion::log::Ed25519Signer stranger_signer(stranger);
  const auto stranger_token = onion::log::issue_invite(
      pubkey_c_hex, stranger.device_id, 3600,
      "3f4e5d6c7b8a8998a9bacbdcedfe0f12", now_s, stranger_signer);
  const auto stranger_response = client.Post(
      "/sync/v1/join", onion::log::invite_to_string(stranger_token),
      "application/json");
  ASSERT_NE(stranger_response, nullptr);
  EXPECT_EQ(stranger_response->status, 401);
}

TEST_F(SyncTest, CheckpointCloneInstallsAnIdenticalStore) {
  // Device A writes data and a checkpoint; a joining device C clones the
  // checkpoint and ends up with the identical node set (Merkle equal).
  writer_a_->submit(CommitTerminal{{
      make_record("ADD_NODE", {{"path", "P01-IN01-T01"},
                               {"layer", 4},
                               {"title", "Order Capture"}}),
  }});
  writer_a_->sync();
  std::string checkpoint_error;
  ASSERT_TRUE(writer_a_->write_checkpoint(&checkpoint_error))
      << checkpoint_error;
  const std::string a_merkle =
      onion::store::merkle_root(writer_a_->db().all_nodes(&checkpoint_error));

  start_server_a();
  const auto key_c = onion::log::DeviceKey::from_seed_hex(
      "303132333435363738393a3b3c3d3e3f"
      "404142434445464748494a4b4c4d4e4f");
  onion::log::Ed25519Signer signer_c(key_c);

  // The joining device is admitted first (the real flow): the invite
  // token signed by B, then the join POST.
  std::string pubkey_c_hex;
  for (const std::uint8_t byte : key_c.public_key) {
    pubkey_c_hex += "0123456789abcdef"[byte >> 4];
    pubkey_c_hex += "0123456789abcdef"[byte & 0x0f];
  }
  {
    onion::log::Ed25519Signer inviter_signer(*key_b_);
    const std::int64_t now_s = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto token = onion::log::issue_invite(
        pubkey_c_hex, key_b_->device_id, 3600,
        "0a1b2c3d4e5f60718293a4b5c6d7e8f9", now_s, inviter_signer);
    httplib::Client join_client(
        ("http://127.0.0.1:" + std::to_string(server_->bound_port()))
            .c_str());
    const auto admission = join_client.Post(
        "/sync/v1/join", onion::log::invite_to_string(token),
        "application/json");
    ASSERT_NE(admission, nullptr);
    ASSERT_EQ(admission->status, 200);
  }

  const auto clone_dir = std::filesystem::temp_directory_path() /
                         ("onion_clone_test_" +
                          std::to_string(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()));
  onion::sync::CloneOptions clone_options{
      key_c.device_id, signer_c, {}, {}};
  clone_options.members = {{key_a_->device_id,
                            {key_a_->device_id, key_a_->public_key}},
                           {key_b_->device_id,
                            {key_b_->device_id, key_b_->public_key}}};
  clone_options.destination = clone_dir;
  const std::string clone_error = onion::sync::clone_checkpoint(
      "127.0.0.1:" + std::to_string(server_->bound_port()), clone_options);
  EXPECT_TRUE(clone_error.empty()) << clone_error;

  onion::store::Db cloned(clone_dir / "onion.db");
  std::string error;
  ASSERT_TRUE(cloned.open(&error)) << error;
  EXPECT_EQ(onion::store::merkle_root(cloned.all_nodes(&error)), a_merkle);
  cloned.close();
  std::filesystem::remove_all(clone_dir);
}

TEST_F(SyncTest, CheckpointCloneRefusesATamperedManifest) {
  writer_a_->submit(CommitTerminal{{
      make_record("ADD_NODE", {{"path", "P01-IN01-T01"},
                               {"layer", 4},
                               {"title", "Order Capture"}}),
  }});
  writer_a_->sync();
  std::string checkpoint_error;
  ASSERT_TRUE(writer_a_->write_checkpoint(&checkpoint_error))
      << checkpoint_error;

  // A tampered manifest: flip one hex digit of the snapshot hash before
  // the clone sees it. The clone verifies the SIGNATURE over the manifest,
  // so the tamper must fail verification.
  const auto manifest_path = [&]() {
    for (const auto& entry :
         std::filesystem::directory_iterator(writer_a_->data_dir())) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("checkpoint-", 0) == 0 &&
          name.size() > 9 && name.substr(name.size() - 9) == ".manifest")
        return entry.path();
    }
    return std::filesystem::path{};
  }();
  std::string manifest_text;
  {
    std::ifstream in(manifest_path, std::ios::binary);
    manifest_text.assign((std::istreambuf_iterator<char>(in)), {});
  }
  nlohmann::json manifest = nlohmann::json::parse(manifest_text);
  std::string hash = manifest["snapshot_hash"].get<std::string>();
  hash[0] = hash[0] == '0' ? '1' : '0';
  manifest["snapshot_hash"] = hash;
  std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
  out << manifest.dump() << "\n";
  out.close();

  start_server_a();
  const auto key_c = onion::log::DeviceKey::from_seed_hex(
      "303132333435363738393a3b3c3d3e3f"
      "404142434445464748494a4b4c4d4e4f");
  onion::log::Ed25519Signer signer_c(key_c);

  // The joining device is admitted first (the real flow): the invite
  // token signed by B, then the join POST.
  std::string pubkey_c_hex;
  for (const std::uint8_t byte : key_c.public_key) {
    pubkey_c_hex += "0123456789abcdef"[byte >> 4];
    pubkey_c_hex += "0123456789abcdef"[byte & 0x0f];
  }
  {
    onion::log::Ed25519Signer inviter_signer(*key_b_);
    const std::int64_t now_s = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto token = onion::log::issue_invite(
        pubkey_c_hex, key_b_->device_id, 3600,
        "0a1b2c3d4e5f60718293a4b5c6d7e8f9", now_s, inviter_signer);
    httplib::Client join_client(
        ("http://127.0.0.1:" + std::to_string(server_->bound_port()))
            .c_str());
    const auto admission = join_client.Post(
        "/sync/v1/join", onion::log::invite_to_string(token),
        "application/json");
    ASSERT_NE(admission, nullptr);
    ASSERT_EQ(admission->status, 200);
  }

  const auto clone_dir = std::filesystem::temp_directory_path() /
                         ("onion_clone_test_" +
                          std::to_string(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()));
  onion::sync::CloneOptions clone_options{
      key_c.device_id, signer_c, {}, {}};
  clone_options.members = {{key_a_->device_id,
                            {key_a_->device_id, key_a_->public_key}}};
  clone_options.destination = clone_dir;
  const std::string clone_error = onion::sync::clone_checkpoint(
      "127.0.0.1:" + std::to_string(server_->bound_port()), clone_options);
  // The tamper must fail at the manifest SIGNATURE check specifically (a
  // vector failure would also be non-empty — pin the exact reason).
  EXPECT_NE(clone_error.find("manifest signature invalid"),
            std::string::npos);
  std::filesystem::remove_all(clone_dir);
}
TEST(SyncGolden, FixedKeyAndRequestGiveFixedSignatureOnEveryOS) {
  // Cross-OS golden (OA-13 work): a fixed key, method, path, body and
  // timestamp must produce byte-identical signatures on Linux, macOS and
  // Windows — any drift in the canonical string-to-sign fails here on the
  // platform that drifts. The expected value was produced once by this
  // implementation and is pinned.
  const auto key = onion::log::DeviceKey::from_seed_hex(
      "000102030405060708090a0b0c0d0e0f"
      "101112131415161718191a1b1c1d1e1f");
  onion::log::Ed25519Signer signer(key);
  const std::string nonce = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
  const std::string signature = onion::sync::sign_request(
      key.device_id, "GET", "/sync/v1/vector", "", nonce, 1700000000,
      signer);
  EXPECT_EQ(key.device_id, "9d24e2eeaf27c2a0");
  EXPECT_EQ(signature.size(), 128u);
  EXPECT_TRUE(onion::sync::request_signature_valid(
      key.device_id, "GET", "/sync/v1/vector", "", nonce, signature,
      key.public_key, 1700000000, 1700000000));
  // The exact pinned bytes:
  EXPECT_EQ(signature, "e6b2f03b22e37545bad7b7399c3a70d9125357b473bb5e81564bc685f7c7a16650a77d36ad123e2acda972ac5cdf8fdd3de616b35987201360e62fa85cd9e00c");
}
}  // namespace