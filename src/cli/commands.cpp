#include "commands.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "api/api.hpp"
#include "core/depot_core.hpp"
#include "gate/gate.hpp"
#include "llm/llm.hpp"
#include "platform/platform.hpp"
#include "sched/sched.hpp"
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

std::unique_ptr<onion::state::StateWriter> open_writer(
    const std::filesystem::path& dir, const std::string& origin,
    std::string* error) {
  onion::state::StateWriter::Options options;
  options.data_dir = dir;
  options.origin = origin;
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
  json campaigns = json::array();
  for (const auto& campaign : writer.campaigns()) {
    json entry;
    entry["id"] = campaign.id;
    entry["state"] = campaign.state;
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
  if (argc < 3 || std::string(argv[2]) != "run") {
    std::cerr << "usage: data_depot node run --data-dir <dir> --base-url <url>"
                 " [--model <id>] [--api-port 8080] [--key-file <path>]\n";
    return 2;
  }
  const std::filesystem::path dir = data_dir_arg(argc, argv);
  const std::string base_url = flag(argc, argv, "--base-url", "");
  const std::string model = flag(argc, argv, "--model", "deepseek-flash");
  const int api_port = std::atoi(flag(argc, argv, "--api-port", "8080").c_str());
  const std::string origin = flag(argc, argv, "--origin", "dev0000000000000001");

  std::string error;
  auto writer = open_writer(dir, origin, &error);
  if (!writer) {
    std::cerr << "cannot open node state: " << error << "\n";
    return 1;
  }
  onion::platform::KeyStore key_store(dir / "keys");
  const std::string token = api_token(key_store);

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

  onion::llm::Client client({base_url, key, 120, 10, 1, 1000});
  onion::llm::Budget budget("1000");
  const onion::gate::SyncAsk ask = sync_ask(client);

  std::atomic<bool> stop_flag{false};

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

  // Engine loop: run every running campaign until stopped.
  onion::sched::Engine engine(
      {*writer, ask, budget, 1, origin, "0.02"});
  while (!stop_flag.load()) {
    bool worked = false;
    for (const auto& campaign : writer->campaigns()) {
      if (campaign.state != "running") continue;
      std::string parse_error;
      json spec_json = json::parse(campaign.spec, nullptr, false);
      auto spec = onion::sched::parse_campaign(spec_json, &parse_error);
      if (!spec) continue;
      const onion::sched::Engine::RunResult result =
          engine.run(*spec, &stop_flag);
      worked = true;
      std::cout << "campaign " << spec->id << " ended: " << result.end_reason
                << " settled=" << result.settled
                << " attempts=" << result.attempts << "\n";
      if (stop_flag.load()) break;
    }
    if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
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
  const std::string origin = flag(argc, argv, "--origin", "dev0000000000000001");
  std::string error;
  auto writer = open_writer(dir, origin, &error);
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
  const std::string origin = flag(argc, argv, "--origin", "dev0000000000000001");
  std::string error;
  auto writer = open_writer(dir, origin, &error);
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
  const std::string origin = flag(argc, argv, "--origin", "dev0000000000000001");
  std::string error;
  auto writer = open_writer(dir, origin, &error);
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
