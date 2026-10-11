#include "commands.hpp"
#include "genesis_import.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <memory>
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
  if (end == nullptr || *end != '\0' || value < 1 || value > 65535)
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
                bool* running) {
  std::string error;
  json result;
  result["node"]["running"] = *running;
  result["node"]["api_port"] = api_port;
  result["spend_usd"] = writer.db().spend_usd(&error);
  // Convergence observables: the applied vector and the Merkle root.
  json vector = json::object();
  for (const auto& [origin, seq] : writer.vector()) vector[origin] = seq;
  result["vector"] = std::move(vector);
  const auto all_nodes = writer.db().all_nodes(&error);
  result["merkle_root"] = onion::store::merkle_root(all_nodes);
  json campaigns = json::array();
  for (const auto& campaign : writer.campaigns()) {
    json entry;
    entry["id"] = campaign.id;
    entry["state"] = campaign.state;
    entry["reserved"] = writer.reserved_total(campaign.id);
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
  json reviews = json::array();
  for (const auto& review : writer.db().open_reviews(&error)) {
    json entry;
    entry["path"] = review.path;
    entry["reason"] = review.reason;
    entry["state"] = review.state;
    reviews.push_back(std::move(entry));
  }
  result["reviews"] = std::move(reviews);
  std::int64_t nodes = 0;
  for (const auto& settle : writer.db().settles(&error)) {
    (void)settle;
    nodes++;
  }
  result["settled_nodes"] = nodes;
  result["lanes"] = json::array({"local"});  // M3: one lane
  return result;
}

}  // namespace

int node_cmd(int argc, char** argv) {
  // --- subcommands that don't run the node ---
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

  // M6: the static member list and the sync server (members.json).
  std::vector<onion::sched::LaneMember> lane_members;
  std::vector<onion::sync::MemberKey> sync_members;
  std::map<std::string, std::string> member_urls;
  std::unique_ptr<onion::sync::SyncServer> sync_server;
  const std::string members_path = flag(argc, argv, "--members", "");
  const std::string self_host = flag(argc, argv, "--host", "127.0.0.1");
  const int self_sync_port = std::atoi(flag(argc, argv, "--sync-port", "7947").c_str());
  if (!members_path.empty()) {
    std::ifstream members_file(members_path);
    auto members_json = json::parse(members_file, nullptr, false);
    if (members_json.is_discarded() || !members_json.is_array()) {
      std::cerr << "--members must point at a JSON array\n";
      return 2;
    }
    for (const auto& member : members_json) {
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
    }
    onion::sync::SyncServer::Options sync_options;
    sync_options.port = self_sync_port;
    sync_options.bind_address = self_host;
    sync_options.writer = writer.get();
    sync_options.key = device_key;
    sync_options.members = sync_members;
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
  const int self_swim_port =
      parse_port(flag(argc, argv, "--swim-port", "7946"), 7946);
  constexpr std::string_view beacon_group = "239.255.77.31";
  std::unique_ptr<onion::swim::Swim> swim;
  std::unique_ptr<onion::swim::UdpTransport> swim_transport;
  if (!lane_members.empty()) {
    onion::swim::Options swim_options;
    swim_options.self_id = device_key->device_id;
    swim_options.self_address =
        self_host + ":" + std::to_string(self_swim_port);
    swim = std::make_unique<onion::swim::Swim>(std::move(swim_options));
    swim_transport = std::make_unique<onion::swim::UdpTransport>();
    std::string swim_error;
    if (!swim_transport->open(static_cast<std::uint16_t>(self_swim_port),
                              std::string(beacon_group), &swim_error)) {
      // Several nodes on ONE machine share UDP 7946 (the multi-process
      // test); the second binder runs without swim instead of dying.
      // Distinct devices on a real LAN each hold the port.
      std::cerr << "swim disabled (udp:" << self_swim_port
                << " unavailable): " << swim_error << "\n";
      swim.reset();
      swim_transport.reset();
    }
    if (swim) {
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
    // Inbound updates must be signed by a known member; failures are
    // rate-limited so a clock-skewed or stale member does not spam.
    swim->set_updates_verifier(
        [&](std::string_view device_id, std::string_view updates_json,
            std::string_view signature_hex, std::int64_t timestamp_s,
            std::int64_t now_s) {
          for (const auto& member : sync_members)
            if (member.device_id == device_id) {
              const bool valid = onion::swim::beacon_signature_valid(
                  device_id, updates_json, signature_hex,
                  member.public_key, timestamp_s, now_s);
              if (!valid) {
                static int failures = 0;
                if (++failures % 100 == 1)
                  std::cerr << "swim: dropped a bad signature from "
                            << device_id << "\n";
              }
              return valid;
            }
          static int unknown = 0;
          if (++unknown % 100 == 1)
            std::cerr << "swim: dropped an update from unknown device "
                      << device_id << "\n";
          return false;
        });
    // The beacon payload (already signed by the engine) goes to the group.
    swim->beacon_hook = [&](std::string payload) {
      swim_transport->send_beacon(payload);
    };
    // Seed the static member list as ACTIVE and the device itself as
    // ACTIVE; addresses arrive through beacons and gossip.
    swim->mutable_membership().set_self_state(
        onion::swim::MemberState::ACTIVE, 1);
    for (const auto& member : sync_members) {
      if (member.device_id == device_key->device_id) continue;
      onion::swim::Member seed;
      seed.device_id = member.device_id;
      seed.state = onion::swim::MemberState::ACTIVE;
      seed.incarnation = 1;
      swim->mutable_membership().upsert(std::move(seed));
    }
    swim->announce();
    std::fprintf(stderr, "swim on udp:%d beacon group %.*s members=%zu\n",
                 self_swim_port, static_cast<int>(beacon_group.size()),
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

  onion::api::Server server(
      {api_port,
       token,
       load_file(std::filesystem::path(ONION_WEB_DIR) / "dashboard.html"),
       writer.get(),
       [&]() {
         bool running = !stop_flag.load();
         return state_json(*writer, api_port, &running).dump();
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
       [&]() { stop_flag.store(true); }});
  if (!server.start(&error)) {
    std::cerr << error << "\n";
    return 1;
  }
  std::cout << "node listening on 127.0.0.1:" << server.bound_port()
            << " data=" << dir << "\n";

  // Engine loop: run every running campaign until stopped. With a member
  // list, the engine works only its own home-lane tickets.
  onion::sched::Engine engine(
      {*writer, ask, budget, 1, device_key->device_id, "0.02", prices,
       device_key->slot, lane_members, device_key->device_id});
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
    // Sync round every 15 s when the member list is present.
    if (sync_server && !lane_members.empty()) {
      static auto last_sync = std::chrono::steady_clock::now();
      static std::uint64_t visits = 0;
      if (std::getenv("ONION_SYNC_DEBUG") && ++visits % 200 == 0)
        std::fprintf(stderr, "sync block heartbeat visits=%llu\n",
                     static_cast<unsigned long long>(visits));
      if (std::chrono::steady_clock::now() - last_sync >=
          std::chrono::seconds(15)) {
        if (std::getenv("ONION_SYNC_DEBUG"))
          std::fprintf(stderr, "sync timer fired, members=%zu\n",
                       lane_members.size());
        last_sync = std::chrono::steady_clock::now();
        for (const auto& member : lane_members) {
          if (member.device_id == device_key->device_id) continue;
          const auto peer_entry = member_urls.find(member.device_id);
          if (peer_entry == member_urls.end()) continue;
          const std::string peer = peer_entry->second;
          std::map<std::string, std::string> quarantined;
          std::string sync_error;
          std::map<std::string, onion::sync::MemberKey> client_members;
          for (const auto& entry : sync_members)
            client_members[entry.device_id] = entry;
          onion::sync::SyncClient client(
              {*writer, device_key, std::move(client_members)});
          const int applied =
              client.run_round(peer, &quarantined, &sync_error);
          if (std::getenv("ONION_SYNC_DEBUG"))
            std::fprintf(stderr, "sync round to %s: applied=%d error=%s\n",
                         peer.c_str(), applied,
                         sync_error.empty() ? "none" : sync_error.c_str());
        }
      }
    }
    // SWIM: one protocol step plus a drain of the UDP socket each loop.
    if (swim && swim_transport) {
      swim->tick();
      swim_transport->poll();
    }
    if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (sync_server) sync_server->stop();
  server.stop();
  writer->stop();
  return 0;
}

int campaign_cmd(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: data_depot campaign start <file> | pause <id> | "
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
    // Cost projection: the measured average attempt cost from the store,
    // or "uncalibrated" until a calibration run exists.
    json projection;
    projection["loop_budget"] = spec->loop_budget;
    projection["average_attempt_cost_usd"] = writer->db().spend_usd(&error);
    projection["note"] =
        "projection uses the device's average attempt cost so far; prices "
        "come from node.conf in M5";
    std::cout << "cost projection: " << projection.dump() << "\n";
    if (!has_flag(argc, argv, "--yes")) {
      std::cerr << "run with --yes to confirm the projection\n";
      return 3;
    }
    writer->submit(onion::state::CommitTerminal{{make_record(
        "CAMPAIGN", {{"id", spec->id}, {"spec", file}, {"state", "running"}})}});
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
    records.push_back(make_record(
        "SETTLE", {{"path", path}, {"kept", kept}, {"atomic", 0},
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
  std::cout << "imported\n";
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
