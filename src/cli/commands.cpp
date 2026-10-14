#include "commands.hpp"
#include "genesis_import.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "api/api.hpp"
#include "core/depot_core.hpp"
#include "profile/profile.hpp"
#include "profile/service.hpp"
#include "sync/sync.hpp"
#include "store/checkpoint.hpp"
#include "gate/gate.hpp"
#include "llm/llm.hpp"
#include "platform/platform.hpp"
#include "sched/sched.hpp"
#include "swim/beacon.hpp"
#include "swim/swim.hpp"
#include "swim/udp_transport.hpp"
#include <httplib.h>
#include "log/invite.hpp"
#include "state/state.hpp"
#include "store/store.hpp"

namespace {

using nlohmann::json;

std::string flag(int argc, char** argv, const std::string& name,
                 const std::string& fallback) {
  for (int index = 0; index < argc; ++index)
    if (std::string(argv[index]) == name && index + 1 < argc)
      return argv[index + 1];
  return fallback;
}

// A TCP/UDP port from a flag: 1..65535, else the fallback.
int parse_port(const std::string& text, int fallback) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  // 0 is a valid request (bind to an ephemeral port).
  if (end == nullptr || *end != '\0' || value < 0 || value > 65535)
    return fallback;
  return static_cast<int>(value);
}

bool has_flag(int argc, char** argv, const std::string& name) {
  for (int index = 0; index < argc; ++index)
    if (std::string(argv[index]) == name) return true;
  return false;
}

std::filesystem::path data_dir_arg(int argc, char** argv) {
  const std::string overridden = flag(argc, argv, "--data-dir", "");
  if (!overridden.empty()) return overridden;
  return onion::platform::data_dir();
}

// Loads the device key from the key store, or generates it on first use.
// The seed is stored as hex; the key never leaves the process.
std::shared_ptr<onion::log::DeviceKey> load_device_key(
    onion::platform::KeyStore& store, std::string* error) {
  const auto stored = store.get("device-key");
  if (stored && stored->size() == 64) {
    return std::make_shared<onion::log::DeviceKey>(
        onion::log::DeviceKey::from_seed_hex(*stored));
  }
  auto key = std::make_shared<onion::log::DeviceKey>(
      onion::log::DeviceKey::generate());
  if (!store.put("device-key", key->seed_hex())) {
    if (error) *error = "cannot store the device key";
    return nullptr;
  }
  return key;
}


#if ONION_CHAOS_HOOKS
namespace {
// Chaos tool (M7): ONION_CLOCK_JUMP_MS adds a fixed offset to the swim
// engine's clock readings (a wall-clock jump on one device).
class JumpClock final : public onion::swim::Clock {
 public:
  explicit JumpClock(std::int64_t jump_ms) : jump_ms_(jump_ms) {}
  std::int64_t now_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() +
           jump_ms_;
  }

 private:
  std::int64_t jump_ms_;
};
}  // namespace
#endif  // ONION_CHAOS_HOOKS
std::unique_ptr<onion::state::StateWriter> open_writer(
    const std::filesystem::path& dir, std::string* error) {
  onion::platform::KeyStore store(dir / "keys");
  auto key = load_device_key(store, error);
  if (!key) return nullptr;
  onion::state::StateWriter::Options options;
  options.data_dir = dir;
  options.origin = key->device_id;
  options.device_key = key;
  options.signer = std::make_unique<onion::log::Ed25519Signer>(*key);
  // Chaos tool (M7): ONION_KILL_AT_COMMIT=1 dies after the batch is
  // durable but before the fold (a power cut); =2 dies after the fold.
#if ONION_CHAOS_HOOKS
  if (std::getenv("ONION_KILL_AT_COMMIT")) {
    const int step = std::atoi(std::getenv("ONION_KILL_AT_COMMIT"));
    options.crash_hook = [step](int hook_step) {
      if (hook_step == step) std::_Exit(137);
    };
  }
#endif  // ONION_CHAOS_HOOKS
  auto writer = std::make_unique<onion::state::StateWriter>(std::move(options));
  if (!writer->start(error)) return nullptr;
  return writer;
}

std::string load_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

onion::gate::SyncAsk sync_ask(onion::llm::Client& client) {
  return [&client](const onion::llm::Request& request) -> onion::llm::Result {
    std::promise<onion::llm::Result> promise;
    auto future = promise.get_future();
    client.submit(request,
                  [&promise](onion::llm::Result result) {
                    promise.set_value(std::move(result));
                  });
    return future.get();
  };
}

std::string api_token(onion::platform::KeyStore& store) {
  const auto existing = store.get("api-token");
  if (existing) return *existing;
  std::string token = "t-" + onion::log::blake2b_hex(
                                  std::to_string(std::chrono::steady_clock::now()
                                                     .time_since_epoch()
                                                     .count()))
                                 .substr(0, 32);
  store.put("api-token", token);
  return token;
}

onion::log::Record make_record(std::string type, nlohmann::json payload) {
  onion::log::Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  return record;
}

json state_json(onion::state::StateWriter& writer, int api_port,
                bool* running, std::uint64_t sync_rounds_completed = 0) {
  std::string error;
  json result;
  result["node"]["running"] = *running;
  result["node"]["api_port"] = api_port;
  // The sync scheduler thread's successful-round counter (M8 pre-work):
  // the convergence tests treat "no new records AND no completed rounds"
  // as the no-progress condition.
  result["sync_rounds_completed"] = sync_rounds_completed;
  // All SQLite reads go through the API handler thread's own read-only
  // connection: a dashboard poll must never serialize behind writer
  // transactions (and never shares a connection across handler threads).
  const onion::store::Db& read = writer.read_db();
  result["spend_usd"] = read.spend_usd(&error);
  // Convergence observables: the applied vector and the Merkle root.
  json vector = json::object();
  for (const auto& [origin, seq] : onion::state::StateWriter::vector_on(read))
    vector[origin] = seq;
  result["vector"] = std::move(vector);
  const auto all_nodes = read.all_nodes(&error);
  result["merkle_root"] = onion::store::merkle_root(all_nodes);
  json campaigns = json::array();
  for (const auto& campaign : writer.campaigns()) {
    json entry;
    entry["id"] = campaign.id;
    entry["state"] = campaign.state;
    entry["reserved"] = writer.reserved_total(campaign.id);
    // The uncapped fold for the budget-overshoot check (decision 0014):
    // at most one chunk per device beyond the budget.
    std::int64_t uncapped = 0;
    for (const auto& [origin, total] :
         writer.reserved_by_origin(campaign.id))
      uncapped += total;
    entry["reserved_uncapped"] = uncapped;
    json reserved_by_origin = json::object();
    for (const auto& [origin, total] :
         writer.reserved_by_origin(campaign.id))
      reserved_by_origin[origin] = total;
    entry["reserved_by_origin"] = std::move(reserved_by_origin);
    try {
      entry["spec"] = json::parse(campaign.spec);
    } catch (const std::exception&) {
      entry["spec"] = json::object();
    }
    campaigns.push_back(std::move(entry));
  }
  result["campaigns"] = std::move(campaigns);
  // Membership as the local node sees it (dashboard + M7 scenarios).
  json members = json::array();
  for (const auto& row : onion::state::StateWriter::members_table_on(read))
    members.push_back({{"device_id", row.device_id},
                       {"state", row.state},
                       {"slot", row.slot}});
  result["members"] = std::move(members);
  json reviews = json::array();
  for (const auto& review : read.open_reviews(&error)) {
    json entry;
    entry["path"] = review.path;
    entry["reason"] = review.reason;
    entry["state"] = review.state;
    reviews.push_back(std::move(entry));
  }
  result["reviews"] = std::move(reviews);
  std::int64_t nodes = 0;
  for (const auto& settle : read.settles(&error)) {
    (void)settle;
    nodes++;
  }
  result["settled_nodes"] = nodes;
  // Forked-log sightings (the push endpoint never applies them; they
  // surface here for the operator).
  json forks = json::array();
  for (const auto& fork : writer.forks())
    forks.push_back({{"origin", fork.origin},
                     {"number", fork.number},
                     {"incoming_hash", fork.incoming_hash},
                     {"stored_hash", fork.stored_hash}});
  result["forks"] = std::move(forks);
  result["lanes"] = json::array({"local"});  // M3: one lane
  return result;
}

}  // namespace

int node_cmd(int argc, char** argv) {
  // --- subcommands that don't run the node ---
  if (argc >= 3 && std::string(argv[2]) == "chaos-hooks") {
    // The scenario tests probe this to decide between running the
    // chaos scenarios (hooks compiled in) and skipping them.
#if ONION_CHAOS_HOOKS
    std::cout << "1\n";
#else
    std::cout << "0\n";
#endif
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "profile") {
    const std::filesystem::path dir = data_dir_arg(argc, argv);
    onion::profile::ScanOptions options;
    options.data_dir = dir.string();
    options.provider_url = flag(argc, argv, "--base-url", "");
    const onion::profile::Scan scan = onion::profile::scan(options);
    const onion::profile::DerivedSettings settings =
        onion::profile::derive(scan, {});
    std::cout << "os=" << scan.os << " cores=" << scan.cores
              << " ram_gb=" << scan.ram_gb
              << " gpu_fp64=" << (scan.gpu_fp64 ? "yes" : "no")
              << " on_battery=" << (scan.on_battery ? (*scan.on_battery ? "yes" : "no")
                                                    : "n/a")
              << "\n";
    std::cout << "base_weight=" << settings.rules.base_weight
              << " start_limit=" << settings.rules.start_limit
              << " ceiling=" << settings.rules.ceiling
              << " cpu_pool=" << settings.rules.cpu_pool
              << " on_battery_cap=" << settings.rules.on_battery_cap
              << " ram_floor_gb=" << settings.rules.ram_floor_gb << "\n";
    // history: the stored profile versions
    onion::store::Db db(dir / "onion.db");
    std::string error;
    if (db.open(&error)) {
      auto stmt = db.prepare(
          "SELECT version, reason, changed, rules_version FROM profiles "
          "ORDER BY version;",
          &error);
      if (stmt) {
        std::string step_error;
        while (stmt->step_row(&step_error))
          std::cout << "profile v" << stmt->integer(0) << " "
                    << stmt->text(1) << " changed=[" << stmt->text(2)
                    << "] rules_v" << stmt->integer(3) << "\n";
      }
    }
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "identity") {
    const std::filesystem::path dir = data_dir_arg(argc, argv);
    onion::platform::KeyStore store(dir / "keys");
    std::string error;
    auto key = load_device_key(store, &error);
    if (!key) {
      std::cerr << error << "\n";
      return 1;
    }
    std::string public_key_hex;
    for (const std::uint8_t byte : key->public_key) {
      public_key_hex += "0123456789abcdef"[byte >> 4];
      public_key_hex += "0123456789abcdef"[byte & 0x0f];
    }
    std::cout << "device_id=" << key->device_id << "\n"
              << "public_key=" << public_key_hex << "\n"
              << "slot=" << key->slot << "\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "join") {
    // M7 joining: present the invitation token to a seed member. The
    // token IS the credential (signed by the inviter); a valid, unused
    // token with a free slot admits the device on the seed, and the
    // MEMBER_ADD record syncs to the rest of the fleet.
    std::string token = flag(argc, argv, "--token", "");
    if (token.empty() && argc >= 4 && argv[3][0] != '-') token = argv[3];
    if (token.size() > 1 && token[0] == '@') {
      std::ifstream file(token.substr(1));
      if (!file) {
        std::cerr << "cannot read the token file\n";
        return 2;
      }
      token = std::string((std::istreambuf_iterator<char>(file)), {});
    }
    const std::string seed = flag(argc, argv, "--seed", "");
    if (token.empty() || seed.empty()) {
      std::cerr << "usage: data_depot node join --token <token|@file> "
                   "--seed <host:port>\n";
      return 2;
    }
    httplib::Client client(("http://" + seed).c_str());
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(10, 0);
    const auto response =
        client.Post("/sync/v1/join", token, "application/json");
    if (!response) {
      std::cerr << "cannot reach the seed at " << seed << "\n";
      return 1;
    }
    std::cout << response->body << "\n";
    if (response->status != 200) return 1;

    // Admitted: the response carries the fleet's member list — write the
    // joiner's members.json, then clone the seed's newest checkpoint
    // (manifest signature, snapshot hash and Merkle root all verified).
    const std::filesystem::path join_dir = data_dir_arg(argc, argv);
    const auto body = json::parse(response->body, nullptr, false);
    if (body.is_object() && body.contains("members")) {
      std::filesystem::create_directories(join_dir);
      const auto tmp_members = join_dir / "members.json.tmp";
      {
        std::ofstream members_file(tmp_members);
        if (!members_file) {
          std::cerr << "cannot write members.json\n";
          return 1;
        }
        // The joined marker: node run starts this device JOINING and
        // turns it ACTIVE once its lag is under 100 ops (DESIGN ›
        // Joining).
        nlohmann::json members_json = {{"members", body["members"]},
                                       {"joined", true}};
        members_file << members_json.dump() << "\n";
      }
      onion::platform::atomic_replace(tmp_members, join_dir / "members.json");
    }
    onion::platform::KeyStore store(join_dir / "keys");
    std::string key_error;
    const auto device_key = load_device_key(store, &key_error);
    if (!device_key) {
      std::cerr << key_error << "\n";
      return 1;
    }
    std::map<std::string, onion::sync::MemberKey> members;
    if (body.is_object() && body.contains("members")) {
      for (const auto& entry : body["members"]) {
        if (!entry.contains("device_id") || !entry.contains("public_key"))
          continue;
        const std::string key_hex = entry["public_key"].get<std::string>();
        std::vector<std::uint8_t> key_bytes = onion::log::unhex_bytes(key_hex);
        if (key_bytes.size() != 32) continue;
        members[entry["device_id"].get<std::string>()] =
            onion::sync::MemberKey{entry["device_id"].get<std::string>(),
                                   std::move(key_bytes)};
      }
    }
    onion::log::Ed25519Signer signer(*device_key);
    onion::sync::CloneOptions clone_options{
        device_key->device_id, signer, std::move(members), join_dir};
    const std::string clone_error =
        onion::sync::clone_checkpoint(seed, clone_options);
    if (!clone_error.empty() &&
        clone_error.find("404") == std::string::npos) {
      std::cerr << "checkpoint clone failed: " << clone_error << "\n";
      return 1;
    }
    std::cout << "joined: members.json written"
              << (clone_error.empty()
                      ? ", checkpoint cloned from " + seed + "\n"
                      : " (no checkpoint yet — the tails will sync at "
                        "run)\n");
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "stop") {
    // Graceful stop of a running node; --leave drains and records
    // MEMBER_LEAVE (DESIGN › Joining › Graceful leave).
    const int port = parse_port(flag(argc, argv, "--api-port", "8080"), 8080);
    const bool leave = has_flag(argc, argv, "--leave");
    httplib::Client client(("http://127.0.0.1:" + std::to_string(port)).c_str());
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(5, 0);
    const auto response = client.Post(
        "/api/stop",
        nlohmann::json{{"leave", leave}}.dump(), "application/json");
    if (!response || response->status != 200) {
      std::cerr << "no running node answered on port " << port << "\n";
      return 1;
    }
    std::cout << (leave ? "leaving" : "stopping") << "\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "evict") {
    // M7 eviction (DESIGN › Eviction and retirement): any member drafts
    // MEMBER_EVICT; it is valid once a majority of members signed it (or
    // the offline admin key). This command builds the record with the
    // local signature plus any collected signatures, and submits it to a
    // running node's API.
    const std::filesystem::path dir = data_dir_arg(argc, argv);
    onion::platform::KeyStore store(dir / "keys");
    std::string error;
    const auto key = load_device_key(store, &error);
    if (!key) {
      std::cerr << error << "\n";
      return 1;
    }
    const std::string device_id = flag(argc, argv, "--device", "");
    if (device_id.empty()) {
      std::cerr << "usage: data_depot node evict --device <id> "
                   "[--sigs-file <f>] [--accept-until <seq>] "
                   "[--api-port <p>]\n";
      return 2;
    }
    const std::int64_t cutoff =
        std::atoll(flag(argc, argv, "--accept-until", "0").c_str());
    nlohmann::json record = {{"type", "MEMBER_EVICT"},
                             {"payload", {{"device_id", device_id},
                                          {"accept_until_seq", cutoff},
                                          {"sigs", json::array()},
                                          {"kind", "majority"}}}};
    // The local signature over the canonical payload (sigs empty).
    {
      nlohmann::json unsigned_payload = record["payload"];
      unsigned_payload.erase("sigs");
      onion::log::Ed25519Signer signer(*key);
      record["payload"]["sigs"].push_back(
          signer.sign(onion::log::canonical_json(unsigned_payload)));
    }
    const std::string sigs_file = flag(argc, argv, "--sigs-file", "");
    if (!sigs_file.empty()) {
      std::ifstream in(sigs_file);
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty()) record["payload"]["sigs"].push_back(line);
      }
    }
    if (has_flag(argc, argv, "--draft")) {
      std::cout << record.dump() << "\n";
      return 0;
    }
    // Submit through the sync server with the signed-request scheme.
    const int port = parse_port(
        flag(argc, argv, "--sync-port", "7947"), 7947);
    httplib::Client client(
        ("http://127.0.0.1:" + std::to_string(port)).c_str());
    client.set_connection_timeout(3, 0);
    client.set_read_timeout(5, 0);
    const std::string body = record.dump();
    onion::log::Ed25519Signer signer(*key);
    httplib::Headers headers;
    for (const auto& [name, value] : onion::sync::signed_post_headers(
             key->device_id, "/sync/v1/evict", body, signer))
      headers.emplace(name, value);
    const auto response =
        client.Post("/sync/v1/evict", headers, body, "application/json");
    if (!response || response->status != 200) {
      std::cerr << "no running node answered on sync port " << port
                << " (" << (response ? std::to_string(response->status)
                                     : "unreachable")
                << ")\n";
      return 1;
    }
    std::cout << response->body << "\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "invite") {
    // M7 joining: print a signed, single-use invitation token
    // (DESIGN › Joining): the invited public key, the inviter, an expiry
    // and a nonce that is recorded when the join completes.
    const std::filesystem::path dir = data_dir_arg(argc, argv);
    onion::platform::KeyStore store(dir / "keys");
    std::string error;
    const auto key = load_device_key(store, &error);
    if (!key) {
      std::cerr << error << "\n";
      return 1;
    }
    const std::string pubkey_hex = flag(argc, argv, "--pubkey", "");
    if (pubkey_hex.size() != 64) {
      std::cerr << "usage: data_depot node invite --pubkey <64-hex> "
                   "[--ttl 24h] [--nonce <hex>]\n";
      return 2;
    }
    const auto ttl =
        onion::log::parse_ttl(flag(argc, argv, "--ttl", "24h"), &error);
    if (!ttl) {
      std::cerr << "bad --ttl: " << error << "\n";
      return 2;
    }
    std::string nonce = flag(argc, argv, "--nonce", "");
    if (nonce.empty()) {
      // 32 fixed hex chars (128 bits) from the random device.
      nonce = onion::sync::fresh_nonce();
    }
    const std::int64_t now_s = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    onion::log::Ed25519Signer signer(*key);
    const auto token = onion::log::issue_invite(
        pubkey_hex, key->device_id, *ttl, nonce, now_s, signer);
    std::cout << onion::log::invite_to_string(token) << "\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "debug-state") {
    // Read-only diagnostic dump: campaigns, open tickets with their lane
    // owners, per-origin reserves, the applied vector and recent attempts.
    const std::filesystem::path dir = data_dir_arg(argc, argv);
    const std::string members_path = flag(argc, argv, "--members", "");
    std::vector<onion::sched::LaneMember> lane_members;
    if (!members_path.empty()) {
      std::ifstream members_file(members_path);
      auto members_json = json::parse(members_file, nullptr, false);
      if (members_json.is_array())
        for (const auto& member : members_json)
          if (member.contains("device_id"))
            lane_members.push_back(
                {member["device_id"].get<std::string>(),
                 member.value("weight", 1.0)});
    }
    onion::store::Db db(dir / "onion.db");
    std::string error;
    if (!db.open(&error)) {
      std::cerr << error << "\n";
      return 1;
    }
    json out;
    json campaigns = json::array();
    for (const auto& campaign : db.campaigns(&error)) {
      json entry;
      entry["id"] = campaign.id;
      entry["state"] = campaign.state;
      try {
        entry["spec"] = json::parse(campaign.spec);
      } catch (const std::exception&) {
        entry["spec"] = json::object();
      }
      // Open tickets for this campaign with their lane owners.
      auto spec = onion::sched::parse_campaign(entry["spec"], &error);
      if (spec) {
        const auto nodes = db.all_nodes(&error);
        std::vector<std::string> reviews;
        for (const auto& review : db.open_reviews(&error))
          if (review.state == "open" || review.state.empty())
            reviews.push_back(review.path);
        json tickets = json::array();
        std::string t_error;
        auto statement = db.prepare(
            "SELECT path FROM settles;", &t_error);
        std::set<std::string> settled;
        if (statement) {
          std::string step_error;
          while (statement->step_row(&step_error))
            settled.insert(statement->text(0));
        }
        for (const auto& ticket : onion::sched::order_tickets(
                 nodes,
                 [&](std::string_view path) { return settled.count(std::string(path)) > 0; },
                 [](std::string_view) { return false; }, reviews, *spec)) {
          json t;
          t["path"] = ticket.path;
          t["layer"] = ticket.layer;
          const auto owner = onion::sched::lane_owner(ticket.path, lane_members);
          t["owner"] = owner ? *owner : "none";
          tickets.push_back(std::move(t));
        }
        entry["open_tickets"] = std::move(tickets);
      }
      campaigns.push_back(std::move(entry));
    }
    out["campaigns"] = std::move(campaigns);
    // Per-origin RESERVE records, scanned from every oplog (read-only).
    {
      json reserves = json::object();
      std::error_code ec;
      const auto oplog = dir / "oplog";
      if (std::filesystem::exists(oplog))
        for (const auto& origin_dir :
             std::filesystem::directory_iterator(oplog, ec)) {
          if (!origin_dir.is_directory()) continue;
          const std::string origin = origin_dir.path().filename().string();
          json per_campaign = json::object();
          for (const auto& file :
               std::filesystem::directory_iterator(origin_dir.path(), ec)) {
            if (!file.is_regular_file()) continue;
            std::ifstream in(file.path(), std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
              if (line.empty()) continue;
              auto batch = json::parse(line, nullptr, false);
              if (batch.is_discarded() || !batch.contains("recs")) continue;
              for (const auto& record : batch["recs"]) {
                if (record.value("t", std::string{}) != "RESERVE") continue;
                const auto& p = record["p"];
                const std::string campaign =
                    p.value("campaign", std::string{});
                per_campaign[campaign] =
                    per_campaign.value(campaign, 0) +
                    p.value("attempts", 0);
              }
            }
          }
          reserves[origin] = std::move(per_campaign);
        }
      out["reserves_by_origin"] = std::move(reserves);
    }
    {
      json vector = json::object();
      auto statement = db.prepare(
          "SELECT origin, seq FROM applied ORDER BY origin;", &error);
      if (statement) {
        std::string step_error;
        while (statement->step_row(&step_error))
          vector[statement->text(0)] = statement->integer(1);
      }
      out["vector"] = std::move(vector);
    }
    {
      auto statement = db.prepare(
          "SELECT COUNT(*) FROM attempts WHERE hlc > "
          "(CAST(strftime('%s','now') AS INTEGER) - 60) * 65536;",
          &error);
      if (statement) {
        std::string step_error;
        if (statement->step_row(&step_error))
          out["attempts_last_60s"] = statement->integer(0);
      }
    }
    out["settled_nodes"] = db.settles(&error).size();
    out["merkle_root"] = onion::store::merkle_root(db.all_nodes(&error));
    std::cout << out.dump() << "\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "key") {
    if (argc < 6 || std::string(argv[3]) != "set") {
      std::cerr << "usage: data_depot node key set <name> <value> "
                   "[--data-dir <dir>]\n";
      return 2;
    }
    const std::filesystem::path dir = data_dir_arg(argc, argv);
    onion::platform::KeyStore store(dir / "keys");
    if (!store.put(argv[4], argv[5])) {
      std::cerr << "cannot store the key\n";
      return 1;
    }
    std::cout << "stored " << argv[4] << "\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[2]) == "install-service") {
    const std::filesystem::path dir = data_dir_arg(argc, argv);
    onion::profile::ServiceConfig config;
    config.binary = flag(argc, argv, "--binary", "");
    config.data_dir = dir.string();
    config.base_url = flag(argc, argv, "--base-url", "");
    config.model = flag(argc, argv, "--model", "deepseek-flash");
    config.api_port = std::atoi(flag(argc, argv, "--api-port", "8080").c_str());
    if (config.binary.empty()) {
      std::cerr << "--binary is required\n";
      return 2;
    }
#ifdef __linux__
    std::cout << onion::profile::systemd_unit(config);
#elif defined(__APPLE__)
    std::cout << onion::profile::launchd_plist(config);
#else
    std::cout << onion::profile::windows_service_script(config);
#endif
    std::cerr << "\ninstall it with the OS tooling (systemd/launchctl/sc) — "
                 "see docs\n";
    return 0;
  }
  if (argc < 3 || std::string(argv[2]) != "run") {
    std::cerr << "usage: data_depot node run --data-dir <dir> --base-url <url>"
                 " [--model <id>] [--api-port 8080] [--key-file <path>]\n";
    return 2;
  }
  const std::filesystem::path dir = data_dir_arg(argc, argv);
  const std::string base_url = flag(argc, argv, "--base-url", "");
  const std::string model = flag(argc, argv, "--model", "deepseek-flash");
  const int api_port = std::atoi(flag(argc, argv, "--api-port", "8080").c_str());

  std::string error;
  auto writer = open_writer(dir, &error);
  if (!writer) {
    std::cerr << "cannot open node state: " << error << "\n";
    return 1;
  }
  onion::platform::KeyStore key_store(dir / "keys");
  const std::string token = api_token(key_store);
  const auto device_key = load_device_key(key_store, &error);
  if (!device_key) {
    std::cerr << error << "\n";
    return 1;
  }

  // The DeepSeek key: environment first (development fallback), then the
  // OS key store. Never printed.
  std::string key;
  if (const char* env = std::getenv("DEEPSEEK_API_KEY")) key = env;
  else {
    const auto stored = key_store.get("deepseek-key");
    if (stored) key = *stored;
  }
  if (key.empty() && !base_url.empty() &&
      base_url.find("127.0.0.1") == std::string::npos &&
      base_url.find("localhost") == std::string::npos) {
    std::cerr << "no DeepSeek key: set DEEPSEEK_API_KEY or store it with "
                 "'node key set'\n";
  }

  // The profile feeds the concurrency defaults: the client's in-flight
  // ceiling starts at the profile's start limit and never exceeds the
  // profile ceiling.
  int profile_limit = 16;
  {
    onion::profile::ScanOptions scan_options;
    scan_options.data_dir = dir.string();
    scan_options.provider_url = base_url;
    const onion::profile::Scan scan = onion::profile::scan(scan_options);
    const onion::profile::DerivedSettings settings =
        onion::profile::derive(scan, {});
    profile_limit = std::min(std::max(settings.rules.start_limit, 1),
                             settings.rules.ceiling);
    std::cout << "profile: base_weight=" << settings.rules.base_weight
              << " start_limit=" << settings.rules.start_limit
              << " ceiling=" << settings.rules.ceiling << "\n";
  }
  onion::llm::Client client(
      {base_url, key, 120, 10, 1, static_cast<std::size_t>(profile_limit)});
  onion::llm::Budget budget("1000");
  const onion::gate::SyncAsk ask = sync_ask(client);

  // Optional per-1M-token prices (USD), peak triplet (decision 0004: no
  // implicit defaults; node.conf arrives in M5).
  std::optional<onion::llm::PriceSchedule> prices;
  const std::string price_in = flag(argc, argv, "--price-in", "");
  const std::string price_cache = flag(argc, argv, "--price-cache", "");
  const std::string price_out = flag(argc, argv, "--price-out", "");
  if (!price_in.empty() || !price_out.empty()) {
    prices = onion::llm::PriceSchedule{
        {price_in.empty() ? "0" : price_in,
         price_cache.empty() ? "0" : price_cache,
         price_out.empty() ? "0" : price_out},
        {price_in.empty() ? "0" : price_in,
         price_cache.empty() ? "0" : price_cache,
         price_out.empty() ? "0" : price_out}};
  }

  std::atomic<bool> stop_flag{false};
  std::atomic<bool> leave_requested{false};

  // M6: the static member list and the sync server (members.json).
  std::vector<onion::sched::LaneMember> lane_members;
  std::vector<onion::sync::MemberKey> sync_members;
  std::map<std::string, std::string> member_urls;
  std::map<std::string, std::string> member_swim_urls;
  std::unique_ptr<onion::sync::SyncServer> sync_server;
  const std::string members_path = flag(argc, argv, "--members", "");
  const std::string self_host = flag(argc, argv, "--host", "127.0.0.1");
  const int self_sync_port = std::atoi(flag(argc, argv, "--sync-port", "7947").c_str());
  const int self_swim_port =
      parse_port(flag(argc, argv, "--swim-port", "7946"), 7946);
  // node join wraps the member list with a "joined" marker: the device
  // starts JOINING and turns ACTIVE once its lag is under 100 ops.
  bool joined_device = false;
  // The swim transport opens first so the sync server's admission
  // response can carry this device's REAL swim (UDP) address as a
  // unicast gossip seed.
  constexpr std::string_view beacon_group = "239.255.77.31";
  std::unique_ptr<onion::swim::Swim> swim;
  std::unique_ptr<onion::swim::UdpTransport> swim_transport;
  int bound_swim_port = 0;
  std::string self_swim_url;
  if (self_swim_port == 0 || !members_path.empty()) {
    // Always open on an ephemeral port when asked, or when a member list
    // exists (gossip addresses must be real, never a guessed default).
  }
  {
    swim_transport = std::make_unique<onion::swim::UdpTransport>();
    std::string swim_error;
    if (swim_transport->open(static_cast<std::uint16_t>(self_swim_port),
                             std::string(beacon_group), &swim_error)) {
      bound_swim_port = *swim_transport->port();
      self_swim_url = self_host + ":" + std::to_string(bound_swim_port);
    } else {
      // Several nodes on ONE machine share UDP 7946 (the multi-process
      // test); the second binder runs without swim instead of dying.
      std::cerr << "swim disabled (udp:" << self_swim_port
                << " unavailable): " << swim_error << "\n";
      swim_transport.reset();
    }
  }
  if (!members_path.empty()) {
    std::ifstream members_file(members_path);
    auto members_json = json::parse(members_file, nullptr, false);
    // Two shapes: a bare member array (the static list) or the object
    // node join writes ({"members": [...], "joined": true}).
    json member_entries = members_json;
    if (members_json.is_discarded() || (!members_json.is_array() &&
                                        !members_json.is_object())) {
      std::cerr << "--members must point at a JSON array\n";
      return 2;
    }
    if (members_json.is_object()) {
      joined_device = members_json.value("joined", false);
      member_entries = members_json.contains("members")
                           ? members_json["members"]
                           : json::array();
    }
    if (!member_entries.is_array()) {
      std::cerr << "--members must point at a JSON array\n";
      return 2;
    }
    for (const auto& member : member_entries) {
      if (!member.contains("device_id") || !member.contains("public_key")) {
        std::cerr << "members.json entries need device_id and public_key\n";
        return 2;
      }
      const std::string member_id = member["device_id"].get<std::string>();
      const std::string public_key_hex = member["public_key"].get<std::string>();
      std::vector<std::uint8_t> public_key;
      for (std::size_t i = 0; i + 1 < public_key_hex.size(); i += 2)
        public_key.push_back(static_cast<std::uint8_t>(
            std::stoul(public_key_hex.substr(i, 2), nullptr, 16)));
      lane_members.push_back(
          {member_id, member.value("weight", 1.0)});
      sync_members.push_back({member_id, public_key});
      if (member.contains("url"))
        member_urls[member_id] = member["url"].get<std::string>();
      if (member.contains("swim"))
        member_swim_urls[member_id] = member["swim"].get<std::string>();
    }
    onion::sync::SyncServer::Options sync_options;
    sync_options.port = self_sync_port;
    sync_options.bind_address = self_host;
    sync_options.writer = writer.get();
    sync_options.key = device_key;
    sync_options.members = sync_members;
    sync_options.member_urls = member_urls;
    sync_options.member_swim_urls = member_swim_urls;
    sync_options.self_swim_url = self_swim_url;
    // self_url stays empty: the server derives its own address from the
    // REAL bound port (the requested port may be 0 = ephemeral).
    sync_server = std::make_unique<onion::sync::SyncServer>(std::move(sync_options));
    std::string sync_error;
    if (!sync_server->start(&sync_error)) {
      std::cerr << sync_error << "\n";
      return 1;
    }
    std::cout << "sync listening on " << self_host << ":"
              << sync_server->bound_port() << "\n";
    std::fprintf(stderr, "sync server started, members=%zu\n",
                 lane_members.size());
  }

  // M7: the SWIM membership engine over UDP (DESIGN › Transport: 7946)
  // and the signed LAN beacon (decision 0013: group 239.255.77.31).
  if (swim_transport) {
    bound_swim_port = *swim_transport->port();
    onion::swim::Options swim_options;
    swim_options.self_id = device_key->device_id;
    // The engine advertises the REAL bound port (port 0 = ephemeral).
    swim_options.self_address =
        self_host + ":" + std::to_string(bound_swim_port);
    swim = std::make_unique<onion::swim::Swim>(std::move(swim_options));
#if ONION_CHAOS_HOOKS
    if (std::getenv("ONION_CLOCK_JUMP_MS")) {
      static JumpClock jump_clock(
          std::atoll(std::getenv("ONION_CLOCK_JUMP_MS")));
      swim->set_clock(&jump_clock);
    }
#endif  // ONION_CHAOS_HOOKS
    {
      swim->set_transport(swim_transport.get());
      swim_transport->set_receiver(
          [&](const std::string& from, std::string datagram) {
            swim->handle_datagram(from, std::move(datagram));
          });
    // Beacon signing with the device key (decision 0013). The signer is
    // captured by value: the lambda outlives this block.
    swim->set_updates_signer(
        [signer = onion::log::Ed25519Signer(*device_key)](
            std::string_view device_id, std::string_view updates_json,
            std::int64_t timestamp_s) mutable {
          return onion::swim::sign_beacon(device_id, updates_json,
                                          timestamp_s, signer);
        });
    // Inbound updates must be signed by a known member (the static list
    // or a device admitted through the join endpoint); failures are
    // rate-limited so a clock-skewed or stale member does not spam.
    swim->set_updates_verifier(
        [&](std::string_view device_id, std::string_view updates_json,
            std::string_view signature_hex, std::int64_t timestamp_s,
            std::int64_t now_s) {
          std::vector<std::uint8_t> public_key;
          for (const auto& member : sync_members)
            if (member.device_id == device_id)
              public_key = member.public_key;
          if (public_key.empty())
            if (const auto stored =
                    writer->member_public_key(device_id))
              public_key = *stored;
          if (public_key.empty()) {
            static int unknown = 0;
            if (++unknown % 100 == 1)
              std::cerr << "swim: dropped an update from unknown device "
                        << device_id << "\n";
            return false;
          }
          const bool valid = onion::swim::beacon_signature_valid(
              device_id, updates_json, signature_hex, public_key,
              timestamp_s, now_s);
          if (!valid) {
            static int failures = 0;
            if (++failures % 100 == 1)
              std::cerr << "swim: dropped a bad signature from "
                        << device_id << "\n";
          }
          return valid;
        });
    // The beacon payload (already signed by the engine) goes to the group.
    swim->beacon_hook = [&](std::string payload) {
      swim_transport->send_beacon(payload);
    };
    // Seed the static member list as ACTIVE; the device itself starts
    // JOINING when it joined through an invitation (ACTIVE once the lag
    // drops below 100 ops), ACTIVE otherwise.
    swim->mutable_membership().set_self_state(
        joined_device ? onion::swim::MemberState::JOINING
                      : onion::swim::MemberState::ACTIVE,
        1);
    // The beacon/gossip carries the SYNC address (host:port) in the
    // member capabilities, so the fleet learns where to pull from —
    // including devices admitted through the join endpoint.
    {
      auto self = swim->mutable_membership().self();
      self.capabilities = {{"sync", self_host + ":" +
                                        std::to_string(
                                            sync_server
                                                ? sync_server->bound_port()
                                                : self_sync_port)}};
      swim->mutable_membership().upsert(std::move(self));
    }
    if (std::getenv("ONION_SYNC_DEBUG"))
      std::fprintf(stderr, "swim self state: %.*s (joined=%d)\n",
                   static_cast<int>(onion::swim::to_string(
                       swim->membership().self().state)
                                       .size()),
                   onion::swim::to_string(swim->membership().self().state)
                       .data(),
                   joined_device ? 1 : 0);
    for (const auto& member : sync_members) {
      if (member.device_id == device_key->device_id) continue;
      onion::swim::Member seed;
      seed.device_id = member.device_id;
      seed.state = onion::swim::MemberState::ACTIVE;
      seed.incarnation = 1;
      // The unicast gossip seed: the member's UDP address (the beacon
      // may be blocked on some networks).
      const auto swim_entry = member_swim_urls.find(member.device_id);
      if (swim_entry != member_swim_urls.end())
        seed.address = swim_entry->second;
      swim->mutable_membership().upsert(std::move(seed));
    }
    swim->announce();
    std::fprintf(stderr, "swim on udp:%d beacon group %.*s members=%zu\n",
                 bound_swim_port, static_cast<int>(beacon_group.size()),
                 beacon_group.data(), swim->membership().size());
    }
  }

  const auto campaign_action = [&](std::string id,
                                   std::string state) -> bool {
    const auto existing = writer->campaign(id);
    if (!existing) return false;
    if (existing->state == "stopped" && state != "stopped") return false;
    json spec = json::parse(existing->spec, nullptr, false);
    if (spec.is_discarded()) spec = json::object();
    writer->submit(onion::state::CommitTerminal{{make_record(
        "CAMPAIGN", {{"id", id}, {"spec", spec}, {"state", state}})}});
    writer->sync();
    return true;
  };

  const auto review_action = [&](std::string path,
                                 std::string decision) -> bool {
    if (!writer->needs_review(path)) return false;
    std::vector<onion::log::Record> records;
    if (decision == "accept") {
      std::string qerror;
      int kept = 0;
      for (const auto& node :
           writer->db().nodes_with_path_prefix(path, &qerror)) {
        const std::size_t dash = node.path.rfind('-');
        const std::string parent =
            dash == std::string::npos ? "" : node.path.substr(0, dash);
        if (parent == path) kept++;
      }
      records.push_back(make_record(
          "REVIEW", {{"path", path}, {"decision", "accept"}, {"note", ""}}));
      records.push_back(make_record(
          "SETTLE", {{"path", path}, {"kept", kept}, {"atomic", 0},
                     {"attempts", 0}, {"score", "0"}, {"end", "children"}}));
    } else if (decision == "retry") {
      records.push_back(make_record(
          "REVIEW", {{"path", path}, {"decision", "retry"}, {"note", ""}}));
      records.push_back(
          make_record("REEXPAND", {{"path", path}, {"reason", ""}}));
    } else {
      std::string qerror;
      for (const auto& node :
           writer->db().nodes_with_path_prefix(path, &qerror))
        records.push_back(make_record(
            "TOMBSTONE", {{"path", node.path}, {"reason", "review rejected"}}));
      records.push_back(make_record(
          "REVIEW", {{"path", path}, {"decision", "reject"}, {"note", ""}}));
    }
    writer->submit(onion::state::CommitTerminal{std::move(records)});
    writer->sync();
    return true;
  };

  // M8 pre-work (the load-starvation fix): the sync round scheduler runs
  // on its own thread with its own 15 s timer, decoupled from the engine
  // loop (DESIGN › Everything runs in one process: sync threads serve
  // and pull while the engine works — a busy device must keep syncing).
  // fleet_mutex guards the member maps the main loop reloads/gossips
  // while the scheduler copies them. The state-writer queue stays the
  // only mutation path.
  std::mutex fleet_mutex;
  std::atomic<std::uint64_t> sync_rounds_completed{0};
  std::atomic<bool> sync_stop{false};
  std::thread sync_thread;

  onion::api::Server server(
      {api_port,
       token,
       load_file(std::filesystem::path(ONION_WEB_DIR) / "dashboard.html"),
       writer.get(),
       [&]() {
         bool running = !stop_flag.load();
         return state_json(*writer, api_port, &running,
                           sync_rounds_completed.load()).dump();
       },
       [&](std::string id, std::string) {
         return campaign_action(std::move(id), "paused");
       },
       [&](std::string id, std::string) {
         return campaign_action(std::move(id), "running");
       },
       [&](std::string id, std::string) {
         return campaign_action(std::move(id), "stopped");
       },
       [&](std::string path, std::string) {
         return review_action(std::move(path), "accept");
       },
       [&](std::string path, std::string) {
         return review_action(std::move(path), "retry");
       },
       [&](std::string path, std::string) {
         return review_action(std::move(path), "reject");
       },
       [&](bool leave) {
         leave_requested.store(leave);
         stop_flag.store(true);
       }});
  if (!server.start(&error)) {
    std::cerr << error << "\n";
    return 1;
  }
  std::cout << "node listening on 127.0.0.1:" << server.bound_port()
            << " data=" << dir << "\n";

  // M7: the device's reachable addresses, written once per run into the
  // log (the deterministic discovery channel — gossip refreshes between
  // runs), then PUSHED to every known peer each sync round so they can
  // pull the rest (a peer cannot pull before it has our address).
  std::string member_addr_batch;
  if (!lane_members.empty()) {
    writer->submit(onion::state::CommitTerminal{{make_record(
        "MEMBER_ADDR",
        {{"device_id", device_key->device_id},
         {"sync", self_host + ":" +
                      std::to_string(sync_server ? sync_server->bound_port()
                                                 : self_sync_port)},
         {"swim", self_host + ":" + std::to_string(bound_swim_port)}})}});
    writer->sync();
    std::ifstream in(writer->segments().active_path());
    std::string line;
    std::string last_line;
    while (std::getline(in, line))
      if (!line.empty()) last_line = line;
    member_addr_batch = std::move(last_line);
  }
  // --port-file: the real bound ports (port 0 = ephemeral), so tests and
  // orchestration never guess or hard-code ports.
  const std::string port_file = flag(argc, argv, "--port-file", "");
  if (!port_file.empty()) {
    nlohmann::json ports = {
        {"api", server.bound_port()},
        {"sync", sync_server ? sync_server->bound_port() : 0},
        {"swim", bound_swim_port},
    };
    std::ofstream out(port_file);
    out << ports.dump();
  }

  // Engine loop: run every running campaign until stopped. With a member
  // list, the engine works only its own home-lane tickets.
  const auto reload_members = [&]() {
    // thread_local: the node loop and the sync scheduler both call this;
    // each thread tracks the file version it has already parsed.
    static thread_local std::filesystem::file_time_type last_write{};
    const auto path = std::filesystem::path(members_path);
    std::error_code ec;
    const auto write = std::filesystem::last_write_time(path, ec);
    if (ec || write == last_write) return;
    last_write = write;
    std::ifstream file(members_path);
    auto parsed = json::parse(file, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return;
    std::vector<onion::sched::LaneMember> new_lanes;
    std::vector<onion::sync::MemberKey> new_members;
    std::map<std::string, std::string> new_urls;
    std::map<std::string, std::string> new_swim_urls;
    for (const auto& member : parsed) {
      if (!member.contains("device_id") || !member.contains("public_key"))
        continue;
      const std::string member_id = member["device_id"].get<std::string>();
      const std::string public_key_hex =
          member["public_key"].get<std::string>();
      std::vector<std::uint8_t> public_key;
      for (std::size_t i = 0; i + 1 < public_key_hex.size(); i += 2)
        public_key.push_back(static_cast<std::uint8_t>(
            std::stoul(public_key_hex.substr(i, 2), nullptr, 16)));
      new_lanes.push_back({member_id, member.value("weight", 1.0)});
      new_members.push_back({member_id, public_key});
      if (member.contains("url"))
        new_urls[member_id] = member["url"].get<std::string>();
      if (member.contains("swim"))
        new_swim_urls[member_id] = member["swim"].get<std::string>();
    }
    {
      std::lock_guard fleet_lock(fleet_mutex);
      lane_members = std::move(new_lanes);
      sync_members = std::move(new_members);
      member_urls = std::move(new_urls);
      member_swim_urls = std::move(new_swim_urls);
      // Read inside the lock: the scheduler thread may swap these maps
      // concurrently (fleet_mutex is the only gate on them).
      if (std::getenv("ONION_SYNC_DEBUG"))
        std::fprintf(stderr, "members.json reloaded: %zu members, %zu urls\n",
                     lane_members.size(), member_urls.size());
    }
  };
  onion::sched::Engine engine(
      {*writer, ask, budget, 1, device_key->device_id, "0.02", prices,
       device_key->slot, lane_members, device_key->device_id});
  // The scheduler: one round at a time, anchored on its own 15 s clock.
  // A round that overruns the interval skips the ticks it missed (logged,
  // never overlapped); everything it touches either copies shared state
  // under fleet_mutex or goes through the state-writer queue.
  const auto run_sync_round = [&]() -> bool {
    // The address book lives with the scheduler: a busy engine must not
    // stop the members.json reload (a peer's url arrives in a rewrite
    // after startup — the two-phase tests and real joins rely on it).
    if (!members_path.empty()) reload_members();
    std::map<std::string, std::string> urls;
    std::vector<onion::sync::MemberKey> members;
    {
      std::lock_guard fleet_lock(fleet_mutex);
      urls = member_urls;
      members = sync_members;
    }
    std::size_t answered_peers = 0;
    // Chaos tool (M7): ONION_PARTITION_FROM=id1,id2 makes this device
    // refuse to talk to the listed peers (a network split).
    std::set<std::string> partitioned;
#if ONION_CHAOS_HOOKS
    if (std::getenv("ONION_PARTITION_FROM")) {
      std::stringstream split(std::getenv("ONION_PARTITION_FROM"));
      std::string id;
      while (std::getline(split, id, ','))
        if (!id.empty()) partitioned.insert(id);
    }
#endif  // ONION_CHAOS_HOOKS
    const std::set<std::string> retired = onion::sync::retired_member_ids(
        onion::state::StateWriter::members_table_on(writer->read_db()));
    for (const auto& [peer_id, peer] : urls) {
      if (peer_id == device_key->device_id) continue;
      if (partitioned.count(peer_id)) continue;
      if (retired.count(peer_id)) continue;  // evicted or departed
      std::map<std::string, std::string> quarantined;
      std::string sync_error;
      std::map<std::string, onion::sync::MemberKey> client_members;
      for (const auto& entry : members)
        client_members[entry.device_id] = entry;
      onion::sync::SyncClient client(
          {*writer, device_key, std::move(client_members)});
      const int applied =
          client.run_round(peer, &quarantined, &sync_error);
      if (sync_error.empty()) ++answered_peers;
      if (std::getenv("ONION_SYNC_DEBUG")) {
        std::string quarantine_note;
        for (const auto& [origin, reason] : quarantined)
          quarantine_note += " " + origin + "->" + reason;
        std::fprintf(stderr,
                     "sync round to %s: applied=%d error=%s%s\n",
                     peer.c_str(), applied,
                     sync_error.empty() ? "none" : sync_error.c_str(),
                     quarantine_note.c_str());
      }
    }
    // Decision 0014 amendment: a round is COMPLETE only when a majority
    // of members (counting self) answered within their timeouts — a
    // minority side of a partition must not unlock more spending, and
    // the completed round re-opens reservations and feeds the
    // measured-chunk window.
    // The denominator is the CURRENT membership: the folded members table
    // (dynamic joins) and the static list — static fleets never write
    // MEMBER_ADD records, so their rows live only in members.json — minus
    // every device the table marks EVICTED or LEFT, plus self.
    std::vector<std::string> listed_ids;
    for (const auto& entry : members) listed_ids.push_back(entry.device_id);
    const std::size_t total_members = onion::sync::majority_denominator(
        listed_ids,
        onion::state::StateWriter::members_table_on(writer->read_db()),
        device_key->device_id);
    const bool majority =
        onion::sync::majority_round(answered_peers, total_members);
    if (std::getenv("ONION_SYNC_DEBUG"))
      std::fprintf(stderr,
                   "sync round verdict: answered=%zu/%zu majority=%s\n",
                   answered_peers + 1, total_members,
                   majority ? "yes" : "no");
    if (majority) {
      engine.note_sync_round();
      ++sync_rounds_completed;
    }
    // M7: push this device's newest batch to every peer (DESIGN › New
    // children and claims are pushed at once). The repeated push also
    // closes the address-discovery circle under load: a peer cannot pull
    // before it has our address, so the push carries it.
    {
      std::ifstream in(writer->segments().active_path());
      std::string line;
      std::string last_line;
      while (std::getline(in, line))
        if (!line.empty()) last_line = line;
      const std::string pushed_batch = std::move(last_line);
      for (const auto& [peer_id, peer] : urls) {
        if (peer_id == device_key->device_id || peer.empty())
          continue;
        if (partitioned.count(peer_id)) continue;  // the split holds
        if (retired.count(peer_id)) continue;  // evicted or departed
        std::map<std::string, onion::sync::MemberKey> client_members;
        for (const auto& entry : members)
          client_members[entry.device_id] = entry;
        onion::sync::SyncClient pusher(
            {*writer, device_key, std::move(client_members),
             self_host + ":" +
                 std::to_string(sync_server ? sync_server->bound_port()
                                            : self_sync_port)});
        std::string push_error;
        if (!member_addr_batch.empty())
          pusher.push_batch(peer, member_addr_batch, &push_error);
        if (!pushed_batch.empty() &&
            pushed_batch != member_addr_batch)
          pusher.push_batch(peer, pushed_batch, &push_error);
        if (std::getenv("ONION_SYNC_DEBUG") && !push_error.empty())
          std::fprintf(stderr, "push to %s failed: %s\n",
                       peer.c_str(), push_error.c_str());
      }
    }
    return answered_peers > 0;
  };
  if (sync_server && !lane_members.empty()) {
    sync_thread = std::thread([&]() {
      auto next_round = std::chrono::steady_clock::now() +
                        std::chrono::seconds(15);
      while (!sync_stop.load()) {
        while (!sync_stop.load() &&
               std::chrono::steady_clock::now() < next_round)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (sync_stop.load()) break;
        const auto started = std::chrono::steady_clock::now();
        if (std::getenv("ONION_SYNC_DEBUG"))
          std::fprintf(stderr, "sync timer fired, members=%zu\n",
                       [&]() {
                         std::lock_guard fleet_lock(fleet_mutex);
                         return lane_members.size();
                       }());
        run_sync_round();
        // Rounds never overlap: a round that overran its 15 s slot
        // skips the ticks it missed (each one logged), so the next
        // round stays anchored on the clock.
        auto scheduled = started + std::chrono::seconds(15);
        while (scheduled <= std::chrono::steady_clock::now()) {
          std::fprintf(stderr, "sync round overran; skipping a tick\n");
          scheduled += std::chrono::seconds(15);
        }
        next_round = scheduled;
      }
    });
  }
  while (!stop_flag.load()) {
    static std::uint64_t loop_visits = 0;
    if (std::getenv("ONION_SYNC_DEBUG") && ++loop_visits % 50 == 0)
      std::fprintf(stderr, "node loop heartbeat visits=%llu\n",
                   static_cast<unsigned long long>(loop_visits));
    bool worked = false;
    for (const auto& campaign : writer->campaigns()) {
      if (campaign.state != "running") continue;
      std::string parse_error;
      json spec_json = json::parse(campaign.spec, nullptr, false);
      auto spec = onion::sched::parse_campaign(spec_json, &parse_error);
      if (!spec) continue;
      if (std::getenv("ONION_SYNC_DEBUG"))
        std::fprintf(stderr, "entering engine.run for %s\n", spec->id.c_str());
      const onion::sched::Engine::RunResult result =
          engine.run(*spec, &stop_flag);
      if (std::getenv("ONION_SYNC_DEBUG"))
        std::fprintf(stderr, "engine.run returned %s\n",
                     result.end_reason.c_str());
      worked = true;
      std::cout << "campaign " << spec->id << " ended: " << result.end_reason
                << " settled=" << result.settled
                << " attempts=" << result.attempts
                << " calls=" << result.calls << "\n";
      // Exhausted and drained lanes are not finished (decision 0010): the
      // loop sleeps instead of spinning until sync brings work or the
      // fleet-wide close arrives.
      if (result.end_reason == "budget" || result.end_reason == "drained")
        worked = false;
      if (stop_flag.load()) break;
    }
    // M7 joining: a JOINING device turns ACTIVE once its lag behind
    // every origin is under 100 ops. The sync round itself lives on
    // the scheduler thread; this check stays on the node loop thread
    // (it mutates SWIM state) with its own 15 s throttle.
    if (swim && swim->membership().self().state ==
                    onion::swim::MemberState::JOINING) {
      static auto last_join_check = std::chrono::steady_clock::now();
      if (std::chrono::steady_clock::now() - last_join_check >=
          std::chrono::seconds(15)) {
        last_join_check = std::chrono::steady_clock::now();
        std::int64_t worst_lag = 0;
        bool lag_known = false;
        std::map<std::string, std::string> urls_snapshot;
        std::vector<onion::sync::MemberKey> members_snapshot;
        {
          std::lock_guard fleet_lock(fleet_mutex);
          urls_snapshot = member_urls;
          members_snapshot = sync_members;
        }
        for (const auto& [peer_id, peer] : urls_snapshot) {
          if (peer_id == device_key->device_id) continue;
          std::map<std::string, onion::sync::MemberKey> client_members;
          for (const auto& entry : members_snapshot)
            client_members[entry.device_id] = entry;
          onion::sync::SyncClient lag_client(
              {*writer, device_key, std::move(client_members),
               self_host + ":" +
                   std::to_string(sync_server ? sync_server->bound_port()
                                                : self_sync_port)});
          std::string lag_error;
          const std::int64_t lag =
              lag_client.peer_lag(peer, &lag_error);
          if (lag < 0) continue;
          lag_known = true;
          worst_lag = std::max(worst_lag, lag);
        }
        if (lag_known && worst_lag < 100) {
          swim->mutable_membership().set_self_state(
              onion::swim::MemberState::ACTIVE,
              swim->membership().self().incarnation + 1);
          swim->announce();
          std::fprintf(stderr, "joined: ACTIVE (lag %lld)\n",
                       static_cast<long long>(worst_lag));
        }
      }
    }
if (!members_path.empty()) reload_members();
    // M7: the automatic retirement proposal (DESIGN › Eviction and
    // retirement) — a member whose origin has written nothing for the
    // absence window (30 days; ONION_EVICT_ABSENT_S shortens it for
    // tests) gets a DRAFT MEMBER_EVICT written to
    // <data-dir>/eviction-proposals/<id>.json. The draft takes effect
    // only once a majority signs it (node evict --sigs-file ...).
    if (swim && swim_transport) {
      static auto last_proposal_check = std::chrono::steady_clock::now();
      const char* absence_env = std::getenv("ONION_EVICT_ABSENT_S");
      const std::int64_t absence_s =
          absence_env ? std::atoll(absence_env)
                      : static_cast<std::int64_t>(30LL * 86400);
      if (std::chrono::steady_clock::now() - last_proposal_check >=
          std::chrono::hours(1)) {
        last_proposal_check = std::chrono::steady_clock::now();
        for (const auto& row : writer->members_table()) {
          if (row.device_id == device_key->device_id) continue;
          if (row.state != "JOINING" && row.state != "ACTIVE" &&
              row.state != "SUSPECT")
            continue;
          const auto origin_dir = dir / "oplog" / row.device_id;
          std::error_code ec;
          if (!std::filesystem::exists(origin_dir)) continue;
          auto latest = std::filesystem::file_time_type::min();
          for (const auto& entry :
               std::filesystem::directory_iterator(origin_dir, ec)) {
            if (!entry.is_regular_file()) continue;
            latest = std::max(latest, entry.last_write_time());
          }
          const auto age = std::filesystem::file_time_type::clock::now() -
                           latest;
          if (age < std::chrono::seconds(absence_s)) continue;
          const auto proposals = dir / "eviction-proposals";
          std::filesystem::create_directories(proposals);
          std::ofstream draft(proposals / (row.device_id + ".json"));
          if (draft) {
            draft << nlohmann::json{{"type", "MEMBER_EVICT"},
                                    {"payload",
                                     {{"device_id", row.device_id},
                                      {"accept_until_seq", 0},
                                      {"sigs", nlohmann::json::array()},
                                      {"kind", "majority"}}}}
                         .dump()
                  << "\n";
            std::fprintf(stderr,
                         "auto-proposal: MEMBER_EVICT drafted for %s "
                         "(absent)\n",
                         row.device_id.c_str());
          }
        }
      }
    }
    // SWIM: one protocol step plus a drain of the UDP socket each loop.
    if (swim && swim_transport) {
      swim->tick();
      swim_transport->poll();
      // M7: the live lane list follows the membership. DEAD, LEFT and
      // EVICTED members drop out ONLY once a majority of the membership
      // is visible — a partitioned device keeps the dead lanes, so a
      // 2-and-2 split moves nothing (DESIGN › Work scheduling ›
      // Eligibility), and stealing never touches a dead lane either.
      std::size_t total = 0, visible = 0;
      for (const auto& member : swim->membership().list())
        if (member.state != onion::swim::MemberState::INVITED) {
          total++;
          if (onion::swim::owns_work(member.state) ||
              member.state == onion::swim::MemberState::JOINING ||
              member.state == onion::swim::MemberState::LEAVING)
            visible++;
        }
      const bool majority_visible = visible * 2 > total;
      if (std::getenv("ONION_SYNC_DEBUG")) {
        static std::size_t last_swim_size = 0;
        if (swim->membership().size() != last_swim_size) {
          last_swim_size = swim->membership().size();
          std::fprintf(stderr, "swim view now %zu members\n",
                       last_swim_size);
        }
      }
      // The gossip seeds: the members.json swim addresses (the two-phase
      // tests and joined fleets learn them after startup). Snapshot
      // under fleet_mutex — the sync scheduler's reload_members() also
      // writes these maps.
      std::map<std::string, std::string> swim_urls_snapshot;
      std::vector<onion::sched::LaneMember> lanes_snapshot;
      {
        std::lock_guard fleet_lock(fleet_mutex);
        swim_urls_snapshot = member_swim_urls;
        lanes_snapshot = lane_members;
      }
      for (const auto& [member_id, swim_url] : swim_urls_snapshot) {
        if (member_id == device_key->device_id) continue;
        const auto* existing = swim->membership().get(member_id);
        if (existing && existing->address == swim_url) continue;
        onion::swim::Member seeded;
        seeded.device_id = member_id;
        seeded.state = existing ? existing->state
                                : onion::swim::MemberState::ACTIVE;
        seeded.incarnation = existing ? existing->incarnation : 1;
        seeded.address = swim_url;
        swim->mutable_membership().upsert(std::move(seeded));
      }
      // The fleet's sync addresses travel in gossip AND in the durable
      // members table (MEMBER_ADDR records): the table is the
      // deterministic fallback, so joined devices are reachable even
      // when the gossip channel is blocked.
      {
        std::lock_guard fleet_lock(fleet_mutex);
        for (const auto& row : writer->members_table()) {
          if (row.device_id == device_key->device_id) continue;
          if (!row.sync_url.empty() &&
              member_urls[row.device_id] != row.sync_url)
            member_urls[row.device_id] = row.sync_url;
          if (!row.swim_url.empty() &&
              member_swim_urls[row.device_id] != row.swim_url)
            member_swim_urls[row.device_id] = row.swim_url;
        }
        for (const auto& member : swim->membership().list()) {
          if (member.device_id == device_key->device_id) continue;
          if (member.capabilities.is_object() &&
              member.capabilities.contains("sync") &&
              member.capabilities["sync"].is_string())
            member_urls[member.device_id] =
                member.capabilities["sync"].get<std::string>();
        }
      }
      // The live lane list INCLUDES this device: lane ownership is the
      // weighted rendezvous over the whole live set, so a device must
      // see itself to own its home lanes (DESIGN › Stealing: "A device
      // works its own lane first and, when the lane runs dry, steals").
      std::vector<onion::sched::LaneMember> live;
      for (const auto& member : swim->membership().list()) {
        const bool alive = onion::swim::owns_work(member.state);
        if (!alive && majority_visible) continue;  // re-hash to survivors
        if (!alive) continue;  // no majority: keep the dead lanes
        auto it = std::find_if(
            lanes_snapshot.begin(), lanes_snapshot.end(),
            [&](const onion::sched::LaneMember& entry) {
              return entry.device_id == member.device_id;
            });
        const double weight =
            it == lanes_snapshot.end() ? 1.0 : it->weight;
        live.push_back({member.device_id, weight});
      }
      engine.set_lane_members(std::move(live));
    }
    if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  // Graceful leave (DESIGN › Joining › Graceful leave): LEAVING was
  // announced when the stop arrived; the in-flight work drained in this
  // loop, so the device now pushes its tail (MEMBER_LEAVE) and goes LEFT.
  if (leave_requested.load() && swim) {
    swim->start_leaving();
    swim->tick();
    if (swim_transport) swim_transport->poll();
  }
  if (leave_requested.load()) {
    writer->submit(onion::state::CommitTerminal{{make_record(
        "MEMBER_LEAVE", {{"device_id", device_key->device_id}})}});
    writer->sync();
    if (swim) swim->confirm_left();
    std::cout << "left: MEMBER_LEAVE written\n";
  }
  sync_stop.store(true);
  if (sync_thread.joinable()) sync_thread.join();
  if (sync_server) sync_server->stop();
  server.stop();
  writer->stop();
  return 0;
}

int campaign_cmd(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: data_depot campaign start <file> | plan <file> | pause <id> | "
                 "resume <id> | stop <id> [--data-dir <dir>] [--yes]\n";
    return 2;
  }
  const std::string action = argv[2];
  const std::filesystem::path dir = data_dir_arg(argc, argv);
  std::string error;
  auto writer = open_writer(dir, &error);
  if (!writer) {
    std::cerr << error << "\n";
    return 1;
  }

  if (action == "plan") {
    if (argc < 4) {
      std::cerr << "campaign plan needs a campaign file\n";
      return 2;
    }
    const json file = json::parse(load_file(argv[3]), nullptr, false);
    if (file.is_discarded()) {
      std::cerr << "campaign file is not valid JSON\n";
      return 2;
    }
    std::string parse_error;
    const auto spec = onion::sched::parse_campaign(file, &parse_error);
    if (!spec) {
      std::cerr << parse_error << "\n";
      return 2;
    }
    const int plan_depth = std::atoi(flag(argc, argv, "--depth-to", "8").c_str());
    const bool one_level = has_flag(argc, argv, "--one-level");
    const int grow_levels = std::atoi(flag(argc, argv, "--grow-levels", "0").c_str());
    onion::sched::PlanParams params =
        onion::sched::measure_params(writer->db(), &error);
    std::vector<std::int64_t> level_nodes;
    const std::string counts = flag(argc, argv, "--from-counts", "");
    if (!counts.empty()) {
      // Explicit per-level starting counts (L0,L1,...): a reproducible
      // corpus projection without a store (the dev import stops at the
      // first duplicate path, so the store may be truncated).
      std::stringstream count_stream(counts);
      std::string item;
      while (std::getline(count_stream, item, ',')) {
        const auto value = std::atoll(item.c_str());
        if (value > 0) level_nodes.push_back(value);
      }
    } else {
      std::string count_error;
      for (const auto& prefix : spec->scope)
        for (const auto& node :
             writer->db().nodes_with_path_prefix(prefix, &count_error)) {
          if (static_cast<std::size_t>(node.layer) >= level_nodes.size())
            level_nodes.resize(node.layer + 1, 0);
          ++level_nodes[node.layer];
        }
      if (level_nodes.empty()) level_nodes.push_back(1);  // a fresh root
    }
    if (grow_levels > 0) {
      std::int64_t total = 0;
      for (const std::int64_t count : level_nodes) total += count;
      std::cout << onion::sched::plan_report(
          onion::sched::plan_grow_levels(total, grow_levels, params), params);
      std::cout << "grow-levels " << grow_levels << " on "
                << total << " existing nodes\n";
    } else {
      std::cout << onion::sched::plan_report(
          onion::sched::plan_campaign(level_nodes, plan_depth, params,
                                      !one_level),
          params);
    }
    return 0;
  }

  if (action == "start") {
    if (argc < 4) {
      std::cerr << "campaign start needs a campaign file\n";
      return 2;
    }
    const json file = json::parse(load_file(argv[3]), nullptr, false);
    if (file.is_discarded()) {
      std::cerr << "campaign file is not valid JSON\n";
      return 2;
    }
    std::string parse_error;
    const auto spec = onion::sched::parse_campaign(file, &parse_error);
    if (!spec) {
      std::cerr << parse_error << "\n";
      return 2;
    }
    if (writer->campaign(spec->id) &&
        writer->campaign(spec->id)->state == "stopped") {
      std::cerr << "campaign " << spec->id << " was stopped; stop is final\n";
      return 1;
    }
    // Cost projection (decision 0016): expansions per level, tokens with the
    // cache split, peak/off-peak cost, time and spend per hour, computed
    // from the store's measured records (defaults where unmeasured).
    const int plan_depth = std::atoi(flag(argc, argv, "--depth-to", "8").c_str());
    const bool one_level = has_flag(argc, argv, "--one-level");
    const int grow_levels =
        std::atoi(flag(argc, argv, "--grow-levels", "0").c_str());
    onion::sched::PlanParams params =
        onion::sched::measure_params(writer->db(), &error);
    std::vector<std::int64_t> level_nodes;
    {
      std::string count_error;
      for (const auto& prefix : spec->scope)
        for (const auto& node :
             writer->db().nodes_with_path_prefix(prefix, &count_error)) {
          if (static_cast<std::size_t>(node.layer) >= level_nodes.size())
            level_nodes.resize(node.layer + 1, 0);
          ++level_nodes[node.layer];
        }
      if (level_nodes.empty()) level_nodes.push_back(1);  // a fresh root
    }
    if (grow_levels > 0) {
      std::int64_t total = 0;
      for (const std::int64_t count : level_nodes) total += count;
      std::cout << onion::sched::plan_report(
          onion::sched::plan_grow_levels(total, grow_levels, params), params);
      std::cout << "grow-levels " << grow_levels << " on "
                << total << " existing nodes\n";
    } else {
      std::cout << onion::sched::plan_report(
          onion::sched::plan_campaign(level_nodes, plan_depth, params),
          params);
    }
    if (!has_flag(argc, argv, "--yes")) {
      std::cerr << "run with --yes to confirm the projection\n";
      return 3;
    }
    writer->submit(onion::state::CommitTerminal{{make_record(
        "CAMPAIGN", {{"id", spec->id},
                     {"spec", onion::sched::campaign_spec_json(*spec)},
                     {"state", "running"}})}});
    writer->sync();
    std::cout << "campaign " << spec->id << " started\n";
    return 0;
  }

  if (action == "pause" || action == "resume" || action == "stop") {
    if (argc < 4) {
      std::cerr << "campaign " << action << " needs an id\n";
      return 2;
    }
    const std::string id = argv[3];
    const auto existing = writer->campaign(id);
    if (!existing) {
      std::cerr << "campaign " << id << " not found\n";
      return 1;
    }
    if (existing->state == "stopped" && action != "stop") {
      std::cerr << "campaign " << id << " is stopped; stop is final\n";
      return 1;
    }
    const std::string state =
        action == "pause" ? "paused" : action == "resume" ? "running" : "stopped";
    writer->submit(onion::state::CommitTerminal{{make_record(
        "CAMPAIGN", {{"id", id},
                     {"spec", json::parse(existing->spec, nullptr, false)},
                     {"state", state}})}});
    writer->sync();
    std::cout << "campaign " << id << " " << state << "\n";
    return 0;
  }
  std::cerr << "unknown campaign action: " << action << "\n";
  return 2;
}

int review_cmd(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: data_depot review list | accept <path> | retry <path>"
                 " | reject <path> [--note <text>] [--data-dir <dir>]\n";
    return 2;
  }
  const std::string action = argv[2];
  const std::filesystem::path dir = data_dir_arg(argc, argv);
  std::string error;
  auto writer = open_writer(dir, &error);
  if (!writer) {
    std::cerr << error << "\n";
    return 1;
  }
  if (action == "list") {
    for (const auto& review : writer->db().open_reviews(&error)) {
      if (review.state != "open" && !review.state.empty()) continue;
      std::cout << review.path << "\t" << review.reason << "\n";
    }
    return 0;
  }
  if (argc < 4) {
    std::cerr << "review " << action << " needs a path\n";
    return 2;
  }
  const std::string path = argv[3];
  const std::string note = flag(argc, argv, "--note", "");
  if (!writer->needs_review(path)) {
    std::cerr << path << " has no open review\n";
    return 1;
  }
  std::vector<onion::log::Record> records;
  if (action == "accept") {
    records.push_back(
        make_record("REVIEW", {{"path", path}, {"decision", "accept"}, {"note", note}}));
    std::string qerror;
    int kept = 0;
    for (const auto& node : writer->db().nodes_with_path_prefix(path, &qerror)) {
      const std::size_t dash = node.path.rfind('-');
      const std::string parent =
          dash == std::string::npos ? "" : node.path.substr(0, dash);
      if (parent == path) kept++;
    }
    // The atomic count comes from the review record (the funnel stored it
    // with the children), not from the store: ATOMIC has no store effect
    // yet in M3.
    int atomic = 0;
    for (const auto& review : writer->db().open_reviews(&qerror))
      if (review.path == path) atomic = review.atomic;
    records.push_back(make_record(
        "SETTLE", {{"path", path}, {"kept", kept}, {"atomic", atomic},
                   {"attempts", 0}, {"score", "0"}, {"end", "children"}}));
  } else if (action == "retry") {
    records.push_back(
        make_record("REVIEW", {{"path", path}, {"decision", "retry"}, {"note", note}}));
    records.push_back(
        make_record("REEXPAND", {{"path", path}, {"reason", note}}));
  } else if (action == "reject") {
    std::string qerror;
    for (const auto& node : writer->db().nodes_with_path_prefix(path, &qerror))
      records.push_back(make_record(
          "TOMBSTONE", {{"path", node.path}, {"reason", "review rejected"}}));
    records.push_back(
        make_record("REVIEW", {{"path", path}, {"decision", "reject"}, {"note", note}}));
  } else {
    std::cerr << "unknown review action: " << action << "\n";
    return 2;
  }
  writer->submit(onion::state::CommitTerminal{std::move(records)});
  writer->sync();
  std::cout << "review " << action << " " << path << "\n";
  return 0;
}

int dev_import_cmd(int argc, char** argv) {
  const std::string root = flag(argc, argv, "--root", "");
  if (root.empty()) {
    std::cerr << "usage: data_depot dev-import --root <corpus-copy> "
                 "[--data-dir <dir>]\n";
    return 2;
  }
  const std::filesystem::path dir = data_dir_arg(argc, argv);
  std::string error;
  auto writer = open_writer(dir, &error);
  if (!writer) {
    std::cerr << error << "\n";
    return 1;
  }
  onion::state::DevImport import;
  for (int level = 0; level <= 9; ++level) {
    char name[64];
    std::snprintf(name, sizeof(name), "Onion - level_%02d.csv", level);
    std::ifstream in(std::filesystem::path(root) / name);
    if (!in) continue;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
      if (depot::trim(line).empty()) continue;
      const auto columns = depot::split_csv(line);
      if (level == 0) {
        if (columns.size() < 2) continue;
        import.nodes.push_back(
            {depot::trim(columns[0]), depot::trim(columns[0]), 0,
             depot::trim(columns[1])});
      } else {
        if (columns.size() < 3) continue;
        import.nodes.push_back(
            {depot::trim(columns[0]), depot::trim(columns[1]),
             level, depot::trim(columns[2])});
      }
    }
  }
  std::cout << "importing " << import.nodes.size() << " nodes (development, "
               "unsigned)\n";
  writer->submit(std::move(import));
  writer->sync();
  const std::size_t skipped = writer->last_import_skipped();
  std::cout << "imported\n";
  if (skipped > 0)
    std::cout << "skipped " << skipped
              << " duplicate-path rows (kept the first row)\n";
  return 0;
}

int export_csv_cmd(int argc, char** argv) {
  const std::string out = flag(argc, argv, "--out", "");
  if (out.empty()) {
    std::cerr << "usage: data_depot export-csv --out <dir> [--data-dir <dir>] "
                 "[--max-layer 9]\n";
    return 2;
  }
  const std::filesystem::path dir = data_dir_arg(argc, argv);
  const int max_layer =
      std::atoi(flag(argc, argv, "--max-layer", "9").c_str());
  std::string error;
  onion::store::Db db(dir / "onion.db");
  if (!db.open(&error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!db.export_csv(out, max_layer, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  std::cout << "exported to " << out << "\n";
  return 0;
}

// ------------------------------------------------------------------ admin

int admin_cmd(int argc, char** argv) {
  const std::string action = argc >= 3 ? argv[2] : "";
  if (action != "keygen") {
    std::cerr << "usage: data_depot admin keygen --out <path>\n";
    return 2;
  }
  const std::string out = flag(argc, argv, "--out", "");
  if (out.empty()) {
    std::cerr << "admin keygen needs --out <path>\n";
    return 2;
  }
  // The admin key must never live inside the data directory.
  const std::filesystem::path target(out);
  const std::filesystem::path data = data_dir_arg(argc, argv);
  const auto inside = [](const std::filesystem::path& path,
                         const std::filesystem::path& base) {
    std::error_code ec;
    const auto canonical_parent =
        std::filesystem::weakly_canonical(path.parent_path(), ec);
    const auto canonical_base = std::filesystem::weakly_canonical(base, ec);
    if (ec) return false;
    const auto relative = canonical_parent.lexically_relative(canonical_base);
    if (relative.empty()) return true;  // the data directory itself
    return !relative.empty() && relative.string().find("..") != 0;
  };
  if (inside(target, data)) {
    std::cerr << "refusing to write the admin key inside the data directory\n";
    return 1;
  }
  if (std::filesystem::exists(target)) {
    std::cerr << target << " already exists; choose another path\n";
    return 1;
  }
  const auto key = onion::log::DeviceKey::generate();
  nlohmann::json file = {{"seed_hex", key.seed_hex()},
                         {"public_key_hex",
                          [&] {
                            std::string hex;
                            for (const std::uint8_t byte : key.public_key)
                              hex += "0123456789abcdef"[byte >> 4],
                                  hex += "0123456789abcdef"[byte & 0x0f];
                            return hex;
                          }()}};
  std::ofstream output(target, std::ios::binary);
  if (!output) {
    std::cerr << "cannot write " << target << "\n";
    return 1;
  }
  output << file.dump() << "\n";
  output.close();
#ifndef _WIN32
  ::chmod(target.c_str(), S_IRUSR | S_IWUSR);
#endif
  std::cout << "public key: " << file["public_key_hex"].get<std::string>() << "\n";
  std::cout << "keep this file offline, outside every machine's data directory\n";
  return 0;
}
