#include "state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include <zstd.h>

#include "log/log.hpp"
#include "store/checkpoint.hpp"

namespace onion::state {
namespace {

// The deterministic fold: one record type, one store mutation. Upserts use
// the highest (hlc, origin) so a replay after a crash is idempotent.
void upsert_node(store::Db& db, const log::Record& record,
                 std::string_view writer_origin, std::string* error) {
  const auto& payload = record.payload;
  const std::string path = payload.at("path").get<std::string>();
  const std::int64_t layer = payload.at("layer").get<std::int64_t>();
  const std::string title = payload.at("title").get<std::string>();
  const std::string code = path.substr(path.rfind('-') + 1);
  const std::size_t dash = path.rfind('-');
  const std::string parent =
      dash == std::string::npos ? std::string{} : path.substr(0, dash);
  const std::string origin = payload.value("origin", std::string(writer_origin));

  auto statement = db.prepare(
      "INSERT INTO nodes(id, code, path, parent_id, layer, title, origin, "
      "hlc) VALUES (COALESCE((SELECT MAX(id) FROM nodes), 0) + 1, ?1, ?2, "
      "(SELECT n2.id FROM nodes n2 WHERE n2.path = ?3), ?4, ?5, ?6, ?7) "
      "ON CONFLICT(path) DO UPDATE SET title=excluded.title, "
      "layer=excluded.layer WHERE excluded.hlc > nodes.hlc OR "
      "(excluded.hlc = nodes.hlc AND excluded.origin > nodes.origin);",
      error);
  if (!statement) return;
  std::string bind_error;
  if (!statement->bind(1, code, &bind_error) ||
      !statement->bind(2, path, &bind_error) ||
      !statement->bind(3, parent, &bind_error) ||
      !statement->bind(4, layer, &bind_error) ||
      !statement->bind(5, title, &bind_error) ||
      !statement->bind(6, origin, &bind_error) ||
      !statement->bind(7, static_cast<std::int64_t>(record.hlc), &bind_error)) {
    if (error) *error = "fold node bind failed: " + bind_error;
    return;
  }
  if (statement->step(&bind_error) || !bind_error.empty()) {
    if (error) *error = "fold node failed: " + bind_error;
  }
}

void upsert_settle(store::Db& db, const log::Record& record,
                   std::string_view writer_origin, std::string* error) {
  const auto& payload = record.payload;
  auto statement = db.prepare(
      "INSERT INTO settles(path, kept, atomic, attempts, score, campaign, "
      "hlc, origin) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
      "ON CONFLICT(path) DO UPDATE SET kept=excluded.kept, "
      "atomic=excluded.atomic, attempts=excluded.attempts, "
      "score=excluded.score, campaign=excluded.campaign, hlc=excluded.hlc, "
      "origin=excluded.origin "
      "WHERE CAST(excluded.score AS REAL) > CAST(settles.score AS REAL) OR "
      "(CAST(excluded.score AS REAL) = CAST(settles.score AS REAL) AND "
      "(excluded.hlc > settles.hlc OR (excluded.hlc = settles.hlc AND "
      "excluded.origin > settles.origin)));",
      error);
  if (!statement) return;
  std::string bind_error;
  if (!statement->bind(1, payload.at("path").get<std::string>(), &bind_error) ||
      !statement->bind(2, payload.value("kept", std::int64_t{0}), &bind_error) ||
      !statement->bind(3, payload.value("atomic", std::int64_t{0}), &bind_error) ||
      !statement->bind(4, payload.value("attempts", std::int64_t{0}), &bind_error) ||
      !statement->bind(5, payload.value("score", std::string{"0"}), &bind_error) ||
      !statement->bind(6, payload.value("campaign", std::string{}), &bind_error) ||
      !statement->bind(7, static_cast<std::int64_t>(record.hlc), &bind_error) ||
      !statement->bind(8, payload.value("origin", std::string(writer_origin)), &bind_error)) {
    if (error) *error = "fold settle bind failed: " + bind_error;
    return;
  }
  if (statement->step(&bind_error) || !bind_error.empty()) {
    if (error) *error = "fold settle failed: " + bind_error;
  }
}

void upsert_attempt(store::Db& db, const log::Record& record,
                    std::string_view writer_origin, std::string* error) {
  const auto& payload = record.payload;
  auto statement = db.prepare(
      "INSERT OR IGNORE INTO attempts(path, n, score, passed, tokens_in, tokens_cached, "
      "tokens_out, cost, campaign, hlc, origin) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);",
      error);
  if (!statement) return;
  std::string bind_error;
  const std::int64_t tokens_in = payload.value("tokens_in", std::int64_t{0});
  const std::int64_t tokens_cached = payload.value("tokens_cached", std::int64_t{0});
  const std::int64_t tokens_out = payload.value("tokens_out", std::int64_t{0});
  if (!statement->bind(1, payload.at("path").get<std::string>(), &bind_error) ||
      !statement->bind(2, payload.value("n", std::int64_t{0}), &bind_error) ||
      !statement->bind(3, payload.value("score", std::string{"0"}), &bind_error) ||
      !statement->bind(4, payload.value("passed", std::int64_t{0}), &bind_error) ||
      !statement->bind(5, tokens_in, &bind_error) ||
      !statement->bind(6, tokens_cached, &bind_error) ||
      !statement->bind(7, tokens_out, &bind_error) ||
      !statement->bind(8, payload.value("cost", std::string{"0"}), &bind_error) ||
      !statement->bind(9, payload.value("campaign", std::string{}), &bind_error) ||
      !statement->bind(10, static_cast<std::int64_t>(record.hlc), &bind_error) ||
      !statement->bind(11, payload.value("origin", std::string(writer_origin)), &bind_error)) {
    if (error) *error = "fold attempt bind failed: " + bind_error;
    return;
  }
  if (statement->step(&bind_error) || !bind_error.empty()) {
    if (error) *error = "fold attempt failed: " + bind_error;
  }
}

void upsert_critique(store::Db& db, const log::Record& record,
                     std::string_view writer_origin, std::string* error) {
  const auto& payload = record.payload;
  // remove/add may be a string or an array of strings; normalize both to a
  // comma-joined string, and never throw on a malformed type (the fold must
  // survive arbitrary records — found by the shuffled-replay test).
  const auto joined = [&](const char* key) {
    if (!payload.contains(key)) return std::string{};
    if (payload[key].is_string()) return payload[key].get<std::string>();
    std::string out;
    if (payload[key].is_array())
      for (const auto& item : payload[key])
        if (item.is_string()) {
          if (!out.empty()) out += ",";
          out += item.get<std::string>();
        }
    return out;
  };
  auto statement = db.prepare(
      // INSERT OR IGNORE keyed on (origin, hlc): the fold must be
      // idempotent — a replayed durable batch must not duplicate the
      // critique (C4).
      "INSERT OR IGNORE INTO critiques(path, score, issues, remove, add_, "
      "hlc, origin) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);",
      error);
  if (!statement) return;
  std::string bind_error;
  if (!statement->bind(1, payload.at("path").get<std::string>(), &bind_error) ||
      !statement->bind(2, payload.value("score", std::string{"0"}), &bind_error) ||
      !statement->bind(3, payload.value("issues", std::string{}), &bind_error) ||
      !statement->bind(4, joined("remove"), &bind_error) ||
      !statement->bind(5, joined("add"), &bind_error) ||
      !statement->bind(6, static_cast<std::int64_t>(record.hlc), &bind_error) ||
      !statement->bind(7, payload.value("origin", std::string(writer_origin)), &bind_error)) {
    if (error) *error = "fold critique bind failed: " + bind_error;
    return;
  }
  if (statement->step(&bind_error) || !bind_error.empty()) {
    if (error) *error = "fold critique failed: " + bind_error;
  }
}

void upsert_review(store::Db& db, const log::Record& record,
                   std::string_view writer_origin, std::string* error) {
  const auto& payload = record.payload;
  const std::string path = payload.at("path").get<std::string>();
  auto statement = db.prepare(
      "INSERT INTO reviews(path, reason, state, note, hlc, origin) "
      "VALUES (?1, ?2, 'open', '', ?3, ?4) "
      "ON CONFLICT(path) DO UPDATE SET state='open', reason=excluded.reason, "
      "hlc=excluded.hlc, origin=excluded.origin "
      "WHERE excluded.hlc > reviews.hlc OR "
      "(excluded.hlc = reviews.hlc AND excluded.origin > reviews.origin);",
      error);
  if (!statement) return;
  std::string bind_error;
  if (!statement->bind(1, path, &bind_error) ||
      !statement->bind(2, payload.value("reason", std::string{}), &bind_error) ||
      !statement->bind(3, static_cast<std::int64_t>(record.hlc), &bind_error) ||
      !statement->bind(4, payload.value("origin", std::string(writer_origin)), &bind_error)) {
    if (error) *error = "fold review bind failed: " + bind_error;
    return;
  }
  if (statement->step(&bind_error) || !bind_error.empty()) {
    if (error) *error = "fold review failed: " + bind_error;
  }
}

void upsert_campaign(store::Db& db, const log::Record& record,
                     std::string_view writer_origin, std::string* error) {
  const auto& payload = record.payload;
  auto statement = db.prepare(
      "INSERT INTO campaigns(id, spec, state, hlc, origin) "
      "VALUES (?1, ?2, ?3, ?4, ?5) "
      "ON CONFLICT(id) DO UPDATE SET spec=excluded.spec, "
      "state=CASE WHEN campaigns.state = 'stopped' THEN 'stopped' "
      "ELSE excluded.state END, hlc=excluded.hlc, origin=excluded.origin "
      "WHERE excluded.hlc > campaigns.hlc OR "
      "(excluded.hlc = campaigns.hlc AND excluded.origin > campaigns.origin);",
      error);
  if (!statement) return;
  std::string bind_error;
  if (!statement->bind(1, payload.at("id").get<std::string>(), &bind_error) ||
      !statement->bind(2, payload.at("spec").dump(), &bind_error) ||
      !statement->bind(3, payload.at("state").get<std::string>(), &bind_error) ||
      !statement->bind(4, static_cast<std::int64_t>(record.hlc), &bind_error) ||
      !statement->bind(5, payload.value("origin", std::string(writer_origin)), &bind_error)) {
    if (error) *error = "fold campaign bind failed: " + bind_error;
    return;
  }
  if (statement->step(&bind_error) || !bind_error.empty()) {
    if (error) *error = "fold campaign failed: " + bind_error;
  }
}

}  // namespace

StateWriter::StateWriter(Options options)
    : options_(std::move(options)),
      db_(options_.data_dir / "onion.db"),
      signer_(options_.signer ? std::move(options_.signer)
                              : std::make_unique<log::NullSigner>()) {}

StateWriter::~StateWriter() { stop(); }

bool StateWriter::start(std::string* error) {
  if (!db_.open(error)) return false;
  log::Segments::Options segment_options;
  segment_options.dir = options_.data_dir / "oplog" / options_.origin;
  segment_options.origin = options_.origin;
  segment_options.seal_bytes = options_.seal_bytes;
  segments_ = std::make_unique<log::Segments>(segment_options);
  if (!segments_->open(error)) return false;
  replay(error);
  if (error && !error->empty()) return false;
  last_checkpoint_ = std::chrono::steady_clock::now();
  started_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

bool StateWriter::write_checkpoint(std::string* error) {
  std::string query_error;
  std::map<std::string, std::int64_t> vector;
  std::optional<store::Statement> stmt = db_.prepare(
      "SELECT origin, seq FROM applied ORDER BY origin;", &query_error);
  if (stmt) {
    std::string step_error;
    while (stmt->step_row(&step_error))
      vector[stmt->text(0)] = stmt->integer(1);
  }
  store::CheckpointWriter writer(
      options_.data_dir,
      [this](std::string_view bytes) {
        return options_.device_key && signer_ ? signer_->sign(bytes)
                                              : std::string{};
      });
  const bool ok = writer.write(hlc_.now(), vector, error);
  applied_since_checkpoint_.store(0);
  last_checkpoint_ = std::chrono::steady_clock::now();
  return ok;
}

namespace {
// zstd-decompresses a sealed segment into memory.
std::string decompress_segment(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string compressed((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  in.close();
  const unsigned long long size =
      ZSTD_getFrameContentSize(compressed.data(), compressed.size());
  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN)
    return {};
  std::string content(static_cast<std::size_t>(size), '\0');
  const std::size_t written = ZSTD_decompress(
      content.data(), content.size(), compressed.data(), compressed.size());
  if (ZSTD_isError(written)) return {};
  return content;
}
}  // namespace

void StateWriter::replay(std::string* error) {
  // Replay every batch of every segment in order, folding each record. The
  // fold is idempotent (highest hlc wins), so a crash between the durable
  // write and the store update recovers exactly. With a device key, each
  // batch is verified against the chain; the first failing line is a torn
  // tail and is cut from the active segment.
  // Walk every origin directory under oplog/ (the own origin and the
  // synced copies), maintaining per-origin chains.
  std::map<std::string, std::vector<std::filesystem::path>> sealed_by_origin;
  std::map<std::string, std::filesystem::path> active_by_origin;
  std::error_code ec;
  const std::filesystem::path oplog = segments_->active_path().parent_path().parent_path();
  if (std::filesystem::exists(oplog))
    for (const auto& origin_dir :
         std::filesystem::directory_iterator(oplog, ec)) {
      if (!origin_dir.is_directory()) continue;
      const std::string origin = origin_dir.path().filename().string();
      for (const auto& entry :
           std::filesystem::directory_iterator(origin_dir.path(), ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".zst")
          sealed_by_origin[origin].push_back(entry.path());
        else if (active_by_origin[origin].empty() ||
                 entry.path() > active_by_origin[origin])
          active_by_origin[origin] = entry.path();
      }
      std::sort(sealed_by_origin[origin].begin(),
                sealed_by_origin[origin].end());
    }
  if (std::getenv("ONION_REPLAY_DEBUG")) {
    std::fprintf(stderr, "replay: %zu origins\n", active_by_origin.size());
  }

  std::vector<std::string> members{options_.origin};

  // process_line verifies one batch against the per-origin chain. For the
  // own origin the full Ed25519 check applies; foreign origins are
  // chain-checked only (their keys arrive with M7 gossip — the batches
  // were signature-verified when they were received).
  const auto process_line = [&](const std::string& line,
                                const std::string& origin,
                                std::string& expected_prev,
                                std::int64_t& expected_first,
                                std::string* line_error) {
    auto batch_json = nlohmann::json::parse(line, nullptr, false);
    if (batch_json.is_discarded() || !batch_json.contains("recs")) {
      if (line_error) *line_error = "unparseable batch line";
      return false;
    }
    log::Batch batch;
    batch.version = batch_json.value("v", 0);
    batch.origin = batch_json.value("o", std::string{});
    batch.number = batch_json.value("b", std::uint64_t{0});
    batch.prev = batch_json.value("prev", std::string{});
    batch.hash = batch_json.value("hash", std::string{});
    batch.sig = batch_json.value("sig", std::string{});
    for (const auto& record_json : batch_json["recs"]) {
      if (!record_json.contains("s") || !record_json.contains("h") ||
          !record_json.contains("t")) {
        if (line_error) *line_error = "malformed record";
        return false;
      }
      batch.records.push_back(log::Record::from_json(record_json));
    }
    if (origin == options_.origin && options_.device_key) {
      log::VerifyContext context;
      context.version = 1;
      context.expected_prev = expected_prev;
      context.expected_first_seq = expected_first;
      context.members = &members;
      context.origin_public_key = &options_.device_key->public_key;
      const log::Verify verified = log::verify(batch, context);
      if (verified != log::Verify::Ok) {
        if (line_error)
          *line_error = "batch failed verification (" +
                        std::to_string(static_cast<int>(verified)) + ")";
        return false;
      }
    } else {
      // Chain-only checks: hash, contiguity, and the prev link (skipped
      // for the unknown batch 1 of a foreign origin).
      if (batch.hash != log::hash_records_hex(batch.records)) {
        if (line_error) *line_error = "bad hash";
        return false;
      }
      if (!batch.records.empty() &&
          batch.records.front().seq != expected_first) {
        if (line_error) *line_error = "gap";
        return false;
      }
      if (batch.number > 1 && batch.prev != expected_prev) {
        if (line_error) *line_error = "bad prev";
        return false;
      }
    }
    for (const log::Record& record : batch.records) {
      if (record.seq >= static_cast<std::int64_t>(next_seq_))
        next_seq_ = static_cast<std::uint64_t>(record.seq) + 1;
      fold(record, batch.origin, error);
    }
    update_applied(batch.origin, batch.records);
    applied_since_checkpoint_ += batch.records.size();
    if (origin == options_.origin) {
      next_batch_ = batch.number + 1;
      prev_hash_ = batch.hash;
    }
    expected_prev = batch.hash;
    expected_first =
        batch.records.empty()
            ? expected_first
            : static_cast<std::int64_t>(batch.records.back().seq) + 1;
    {
      std::unique_lock chain_lock(chain_mutex_);
      origin_prev_hashes_[origin] = batch.hash;
    }
    return true;
  };

  const auto origins = [&]() {
    std::set<std::string> all;
    for (const auto& [origin, files] : sealed_by_origin) all.insert(origin);
    for (const auto& [origin, file] : active_by_origin) all.insert(origin);
    return all;
  }();

  for (const std::string& origin : origins) {
    // One chain per origin: sealed segments first, then the active one,
    // sharing the same expected_prev / expected_first.
    std::string expected_prev =
        origin == options_.origin && options_.device_key
            ? log::genesis_prev(*options_.device_key)
            : std::string{};
    std::int64_t expected_first = 1;

    auto sealed = sealed_by_origin.find(origin);
    if (sealed != sealed_by_origin.end()) {
      for (const auto& file : sealed->second) {
        const std::string content = decompress_segment(file);
        if (content.empty()) {
          if (error)
            *error = "cannot decompress sealed segment " + file.string();
          return;
        }
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line)) {
          if (line.empty()) continue;
          std::string line_error;
          if (!process_line(line, origin, expected_prev, expected_first,
                            &line_error)) {
            if (error)
              *error = "sealed segment " + file.string() +
                       " failed verification: " + line_error +
                       " (corruption, not a torn tail)";
            return;
          }
        }
      }
    }

    auto active = active_by_origin.find(origin);
    if (active != active_by_origin.end()) {
      const std::filesystem::path& active_file = active->second;
      std::ifstream in(active_file, std::ios::binary);
      std::vector<std::string> lines;
      std::string line;
      while (std::getline(in, line)) lines.push_back(line);
      in.close();
      std::size_t good = 0;
      for (; good < lines.size(); ++good) {
        if (lines[good].empty()) continue;
        std::string line_error;
        if (!process_line(lines[good], origin, expected_prev, expected_first,
                          &line_error))
          break;
      }
      // A failing tail line is a torn tail: cut it (and anything after).
      if (good < lines.size()) {
        std::uint64_t keep = 0;
        for (std::size_t i = 0; i < good; ++i) keep += lines[i].size() + 1;
        std::filesystem::resize_file(active_file, keep, ec);
        if (ec) {
          if (error) *error = "cannot cut torn tail: " + ec.message();
          return;
        }
        std::fprintf(stderr,
                     "state writer: cut a torn tail of %zu batch(es) from %s\n",
                     lines.size() - good, active_file.string().c_str());
      }
    }
  }
  rebuild_index();
}

void StateWriter::rebuild_index() {
  std::string error;
  std::unique_lock lock(index_mutex_);
  settled_paths_.clear();
  leaf_paths_.clear();
  review_paths_.clear();
  campaigns_.clear();
  for (const store::Settle& settle : db_.settles(&error))
    settled_paths_.insert(settle.path);
  for (const store::Review& review : db_.open_reviews(&error)) {
    if (review.state == "open" || review.state.empty())
      review_paths_.insert(review.path);
  }
  for (const store::Campaign& campaign : db_.campaigns(&error)) {
    campaigns_.push_back(CampaignInfo{campaign.id, campaign.spec,
                                      campaign.state, campaign.hlc,
                                      campaign.origin});
    const auto spec = nlohmann::json::parse(campaign.spec, nullptr, false);
    if (spec.is_object() && spec.contains("loop_budget") &&
        spec["loop_budget"].is_number_integer())
      campaign_budgets_[campaign.id] = spec["loop_budget"].get<std::int64_t>();
  }
  // The RESERVE fold is replayed from the log before this point (replay()
  // folds every record); RESERVE has no store table, so these maps must
  // survive the rebuild — clearing them here erased the fleet budget
  // accounting on every restart (decision 0014).
  // Leaves: settled nodes whose end is not "children". The settles table
  // does not store end (DESIGN schema), so it is re-derived from the log.
  std::error_code ec;
  const std::filesystem::path dir = segments_->active_path().parent_path();
  if (!std::filesystem::exists(dir)) return;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      auto batch_json = nlohmann::json::parse(line, nullptr, false);
      if (batch_json.is_discarded() || !batch_json.contains("recs")) continue;
      for (const auto& record_json : batch_json["recs"]) {
        if (!record_json.contains("t") || record_json["t"] != "SETTLE") continue;
        const auto& payload = record_json["p"];
        if (!payload.contains("path")) continue;
        const std::string path = payload["path"].get<std::string>();
        const std::string end = payload.value("end", std::string{"children"});
        if (end != "children") leaf_paths_.insert(path);
      }
    }
  }
}

void StateWriter::fold(const log::Record& record, std::string_view batch_origin,
                       std::string* error) {
  const std::string& type = record.type;
  const std::string origin(batch_origin);
  if (type == "ADD_NODE") upsert_node(db_, record, origin, error);
  else if (type == "SETTLE") upsert_settle(db_, record, origin, error);
  else if (type == "ATTEMPT") upsert_attempt(db_, record, origin, error);
  else if (type == "CRITIQUE") upsert_critique(db_, record, origin, error);
  else if (type == "NEEDS_REVIEW") upsert_review(db_, record, origin, error);
  else if (type == "CAMPAIGN") upsert_campaign(db_, record, origin, error);
  // REVIEW decisions update the reviews row:
  else if (type == "REVIEW") {
    const auto& payload = record.payload;
    auto statement = db_.prepare(
        "UPDATE reviews SET state='decided', note=?2 WHERE path=?1;", error);
    if (statement) {
      std::string bind_error;
      statement->bind(1, payload.at("path").get<std::string>(), &bind_error);
      statement->bind(2, payload.value("note", std::string{}), &bind_error);
      statement->step(&bind_error);
    }
  } else if (type == "TOMBSTONE") {
    const auto& payload = record.payload;
    auto statement = db_.prepare(
        "UPDATE nodes SET tombstone=1 WHERE path=?1;", error);
    if (statement) {
      std::string bind_error;
      statement->bind(1, payload.at("path").get<std::string>(), &bind_error);
      statement->step(&bind_error);
    }
  } else if (type == "RESERVE") {
    const auto& payload = record.payload;
    const std::string campaign = payload.value("campaign", std::string{});
    const std::int64_t attempts = payload.value("attempts", std::int64_t{0});
    const std::string key = origin + ":" + std::to_string(record.hlc);
    std::unique_lock lock(index_mutex_);
    if (seen_reserve_keys_.insert(key).second) {
      reserved_by_campaign_[campaign] += attempts;
      reserved_by_origin_campaign_[origin + "|" + campaign] += attempts;
      reserve_entries_[campaign].push_back(
          ReserveEntry{record.hlc, origin, attempts});
    }
  } else if (type == "MEMBER_PROFILE") {
    const auto& payload = record.payload;
    auto statement = db_.prepare(
        "INSERT OR IGNORE INTO profiles(device_id, version, prev, reason, changed, "
        "spec, rules_version, schema, hlc) VALUES (?1, ?2, ?3, ?4, ?5, ?6, "
        "?7, ?8, ?9);",
        error);
    if (statement) {
      std::string bind_error;
      std::string changed;
      if (payload.contains("changed") && payload["changed"].is_array())
        for (const auto& item : payload["changed"])
          if (item.is_string()) {
            if (!changed.empty()) changed += ",";
            changed += item.get<std::string>();
          }
      if (!statement->bind(1, payload.value("device_id", std::string{options_.origin}), &bind_error) ||
          !statement->bind(2, payload.value("version", std::int64_t{1}), &bind_error) ||
          !statement->bind(3, payload.value("prev", std::string{}), &bind_error) ||
          !statement->bind(4, payload.value("reason", std::string{}), &bind_error) ||
          !statement->bind(5, changed, &bind_error) ||
          !statement->bind(6, payload.value("spec", nlohmann::json::object()).dump(), &bind_error) ||
          !statement->bind(7, payload.value("rules_version", std::int64_t{1}), &bind_error) ||
          !statement->bind(8, payload.value("schema", std::int64_t{1}), &bind_error) ||
          !statement->bind(9, static_cast<std::int64_t>(record.hlc), &bind_error)) {
        if (error) *error = "fold profile bind failed: " + bind_error;
        return;
      }
      statement->step(&bind_error);
    }
  }
  // ATOMIC, RESERVE, CLAIM, REEXPAND, RETITLE, TOMBSTONE, membership and
  // CHECKPOINT records are append-only in M3; their store effects arrive
  // with their milestones.

  // Keep the in-memory index current without a full rebuild.
  std::unique_lock lock(index_mutex_);
  if (type == "SETTLE") {
    const std::string path = record.payload.at("path").get<std::string>();
    settled_paths_.insert(path);
    const std::string end = record.payload.value("end", std::string{"children"});
    if (end != "children") leaf_paths_.insert(path);
  } else if (type == "NEEDS_REVIEW") {
    const std::string path = record.payload.at("path").get<std::string>();
    review_paths_.insert(path);
  } else if (type == "REVIEW") {
    const std::string path = record.payload.at("path").get<std::string>();
    review_paths_.erase(path);
  } else if (type == "CAMPAIGN") {
    const auto& payload = record.payload;
    const std::string id = payload.at("id").get<std::string>();
    std::string state = payload.at("state").get<std::string>();
    auto it = std::find_if(campaigns_.begin(), campaigns_.end(),
                           [&](const CampaignInfo& c) { return c.id == id; });
    // Latest (hlc, origin) wins, in the index exactly as in the store
    // (upsert_campaign's WHERE): a stale record delivered late must not
    // move the campaign back (decision 0014's capped fold reads this).
    const bool newer =
        it == campaigns_.end() || record.hlc > it->hlc ||
        (record.hlc == it->hlc && origin > it->origin);
    if (!newer) return;
    // stop is final, in the index as in the store.
    if (it != campaigns_.end() && it->state == "stopped") state = "stopped";
    campaigns_.erase(std::remove_if(campaigns_.begin(), campaigns_.end(),
                                    [&](const CampaignInfo& c) { return c.id == id; }),
                     campaigns_.end());
    campaigns_.push_back(CampaignInfo{id, payload.at("spec").dump(), state,
                                      record.hlc, origin});
    // Decision 0014: the capped fold uses the LATEST spec's loop budget.
    const auto spec = payload.at("spec");
    if (spec.is_object() && spec.contains("loop_budget") &&
        spec["loop_budget"].is_number_integer())
      campaign_budgets_[id] = spec["loop_budget"].get<std::int64_t>();
  }
}

bool StateWriter::append_batch(std::vector<log::Record> records,
                               std::string* error, int hook_step_base) {
  if (records.empty()) return true;
  for (log::Record& record : records) {
    if (record.seq == 0) record.seq = static_cast<std::int64_t>(next_seq_.load());
    ++next_seq_;
    if (record.hlc == 0) record.hlc = hlc_.now();
  }
  log::Batch batch = log::make_batch(
      options_.origin, next_batch_.load(),
      prev_hash_.empty() && options_.device_key
          ? log::genesis_prev(*options_.device_key)
          : prev_hash_,
      records, *signer_);
  ++next_batch_;
  if (!segments_->append(batch, error)) return false;
  prev_hash_ = batch.hash;
  if (options_.crash_hook)
    options_.crash_hook(hook_step_base);  // batch durable, store stale
  for (const log::Record& record : records) fold(record, options_.origin, error);
  update_applied(options_.origin, records);
  if (error && !error->empty()) return false;
  if (options_.crash_hook)
    options_.crash_hook(hook_step_base + 1);  // fold done
  return true;
}

void StateWriter::update_applied(std::string_view origin,
                                 const std::vector<log::Record>& records) {
  if (records.empty()) return;
  std::string error;
  auto statement = db_.prepare(
      "INSERT INTO applied(origin, seq) VALUES (?1, ?2) "
      "ON CONFLICT(origin) DO UPDATE SET seq = excluded.seq "
      "WHERE excluded.seq > applied.seq;",
      &error);
  if (!statement) return;
  std::string bind_error;
  if (!statement->bind(1, std::string(origin), &bind_error) ||
      !statement->bind(2, records.back().seq, &bind_error)) {
    return;
  }
  statement->step(&bind_error);
}

std::map<std::string, std::int64_t> StateWriter::vector() const {
  std::map<std::string, std::int64_t> result;
  std::string error;
  auto statement = db_.prepare(
      "SELECT origin, seq FROM applied ORDER BY origin;", &error);
  if (!statement) return result;
  std::string step_error;
  while (statement->step_row(&step_error))
    result[statement->text(0)] = statement->integer(1);
  return result;
}

bool StateWriter::flush_pending_attempts(std::string* error) {
  if (pending_attempts_.empty()) return true;
  std::vector<log::Record> records;
  records.swap(pending_attempts_);
  last_flush_ = std::chrono::steady_clock::now();
  // Step base 3: the attempt-durable and attempt-folded kill points, kept
  // distinct from the terminal batch's 1/2 (C4 commit-order coverage).
  return append_batch(std::move(records), error, 3);
}

void StateWriter::run() {
  for (;;) {
    Command command;
    {
      std::unique_lock lock(queue_mutex_);
      queue_cv_.wait_for(lock, std::chrono::milliseconds(250), [this] {
        return stopping_ || !queue_.empty();
      });
      if (queue_.empty()) {
        if (stopping_) {
          std::string error;
          flush_pending_attempts(&error);
          synced_flag_.store(true);
          synced_cv_.notify_all();
          return;
        }
        // Group-commit: flush ATTEMPT records at least once per second.
        std::string error;
        if (std::chrono::steady_clock::now() - last_flush_ >=
                std::chrono::seconds(1) &&
            !pending_attempts_.empty())
          flush_pending_attempts(&error);
        continue;
      }
      command = std::move(queue_.front());
      queue_.pop_front();
    }

    std::string error;
    switch (command.kind) {
      case Command::Kind::Attempt:
        pending_attempts_.push_back(command.attempt);
        if (pending_attempts_.size() >= 64) flush_pending_attempts(&error);
        break;
      case Command::Kind::Terminal:
        flush_pending_attempts(&error);
        if (error.empty()) append_batch(std::move(command.records), &error);
        break;
      case Command::Kind::Foreign: {
        // Append the verified foreign batch to the origin's local log and
        // fold it. The batch was verified by the sync client.
        flush_pending_attempts(&error);
        if (!error.empty()) break;
        // Append to the foreign origin's segments (a separate Segments).
        auto& foreign_segments = foreign_segments_[command.foreign.origin];
        if (!foreign_segments) {
          log::Segments::Options segment_options;
          segment_options.dir =
              options_.data_dir / "oplog" / command.foreign.origin;
          segment_options.origin = command.foreign.origin;
          segment_options.seal_bytes = options_.seal_bytes;
          foreign_segments =
              std::make_unique<log::Segments>(segment_options);
          std::string open_error;
          foreign_segments->open(&open_error);
        }
        std::string append_error;
        if (!foreign_segments->append(command.foreign, &append_error)) {
          error = "foreign append failed: " + append_error;
          break;
        }
        for (const log::Record& record : command.foreign.records)
          fold(record, command.foreign.origin, &error);
        update_applied(command.foreign.origin, command.foreign.records);
        {
          std::unique_lock chain_lock(chain_mutex_);
          origin_prev_hashes_[command.foreign.origin] = command.foreign.hash;
        }
        break;
      }
      case Command::Kind::Import: {
        auto statement = db_.prepare(
            "INSERT INTO nodes(code, path, parent_id, layer, title, origin, "
            "hlc) VALUES (?1, ?2, (SELECT n2.id FROM nodes n2 WHERE n2.path = "
            "?3), ?4, ?5, 'dev-import', ?6);",
            &error);
        if (!statement) break;
        for (const DevImportedNode& node : command.nodes) {
          std::string bind_error;
          const std::size_t dash = node.path.rfind('-');
          const std::string parent = dash == std::string::npos
                                         ? std::string{}
                                         : node.path.substr(0, dash);
          if (!statement->bind(1, node.code, &bind_error) ||
              !statement->bind(2, node.path, &bind_error) ||
              !statement->bind(3, parent, &bind_error) ||
              !statement->bind(4, static_cast<std::int64_t>(node.layer), &bind_error) ||
              !statement->bind(5, node.title, &bind_error) ||
              !statement->bind(6, static_cast<std::int64_t>(hlc_.now()), &bind_error)) {
            error = "import bind failed: " + bind_error;
            break;
          }
          if (statement->step(&bind_error) || !bind_error.empty()) {
            error = "import step failed: " + bind_error;
            break;
          }
          statement->reset(&bind_error);
        }
        break;
      }
      case Command::Kind::Sync:
        flush_pending_attempts(&error);
        synced_flag_.store(true);
        synced_cv_.notify_all();
        break;
      case Command::Kind::Shutdown:
        flush_pending_attempts(&error);
        synced_flag_.store(true);
        synced_cv_.notify_all();
        return;
    }
    if (!error.empty()) {
      // A fold or append failure must not be silent.
      std::fprintf(stderr, "state writer error: %s\n", error.c_str());
    }
  }
}

void StateWriter::submit(AppendAttempt command) {
  std::lock_guard lock(queue_mutex_);
  Command queued;
  queued.kind = Command::Kind::Attempt;
  queued.attempt = std::move(command.attempt);
  queue_.push_back(std::move(queued));
  queue_cv_.notify_one();
}

void StateWriter::submit(CommitTerminal command) {
  std::lock_guard lock(queue_mutex_);
  Command queued;
  queued.kind = Command::Kind::Terminal;
  queued.records = std::move(command.records);
  queue_.push_back(std::move(queued));
  queue_cv_.notify_one();
}

void StateWriter::submit(DevImport command) {
  std::lock_guard lock(queue_mutex_);
  Command queued;
  queued.kind = Command::Kind::Import;
  queued.nodes = std::move(command.nodes);
  queue_.push_back(std::move(queued));
  queue_cv_.notify_one();
}

void StateWriter::submit(AppendForeign command) {
  std::lock_guard lock(queue_mutex_);
  Command queued;
  queued.kind = Command::Kind::Foreign;
  queued.foreign = std::move(command.batch);
  queue_.push_back(std::move(queued));
  queue_cv_.notify_one();
}

void StateWriter::sync() {
  synced_flag_.store(false);
  {
    std::lock_guard lock(queue_mutex_);
    Command queued;
    queued.kind = Command::Kind::Sync;
    queue_.push_back(std::move(queued));
    queue_cv_.notify_one();
  }
  std::unique_lock lock(queue_mutex_);
  synced_cv_.wait(lock, [this] { return synced_flag_.load(); });
}

void StateWriter::stop() {
  if (!started_) {
    if (thread_.joinable()) thread_.join();
    return;
  }
  synced_flag_.store(false);
  {
    std::lock_guard lock(queue_mutex_);
    Command queued;
    queued.kind = Command::Kind::Shutdown;
    queue_.push_back(std::move(queued));
    stopping_ = true;
    queue_cv_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
  started_ = false;
  // Release the database and every segment handle so the data directory
  // can be removed (Windows keeps open files locked).
  if (segments_) segments_->close();
  for (auto& [origin, foreign] : foreign_segments_)
    if (foreign) foreign->close();
  foreign_segments_.clear();
  db_.close();
}

const store::Db& StateWriter::db() const { return db_; }
log::Segments& StateWriter::segments() { return *segments_; }

bool StateWriter::settled(std::string_view path) const {
  std::shared_lock lock(index_mutex_);
  return settled_paths_.count(std::string(path)) != 0;
}

bool StateWriter::leaf(std::string_view path) const {
  std::shared_lock lock(index_mutex_);
  return leaf_paths_.count(std::string(path)) != 0;
}

bool StateWriter::needs_review(std::string_view path) const {
  std::shared_lock lock(index_mutex_);
  return review_paths_.count(std::string(path)) != 0;
}

std::optional<CampaignInfo> StateWriter::campaign(std::string_view id) const {
  std::shared_lock lock(index_mutex_);
  for (const CampaignInfo& campaign : campaigns_)
    if (campaign.id == id) return campaign;
  return std::nullopt;
}

std::vector<CampaignInfo> StateWriter::campaigns() const {
  std::shared_lock lock(index_mutex_);
  return campaigns_;
}

std::int64_t StateWriter::campaign_budget(std::string_view campaign) const {
  const auto found = campaign_budgets_.find(std::string(campaign));
  return found == campaign_budgets_.end()
             ? std::numeric_limits<std::int64_t>::max()
             : found->second;
}

std::vector<StateWriter::ReserveEntry> StateWriter::reserve_entries_sorted(
    std::string_view campaign) const {
  std::shared_lock lock(index_mutex_);
  const auto found = reserve_entries_.find(std::string(campaign));
  if (found == reserve_entries_.end()) return {};
  std::vector<ReserveEntry> sorted = found->second;
  std::sort(sorted.begin(), sorted.end(),
            [](const ReserveEntry& a, const ReserveEntry& b) {
              if (a.hlc != b.hlc) return a.hlc < b.hlc;
              return a.origin < b.origin;
            });
  return sorted;
}

std::int64_t StateWriter::reserved_total(std::string_view campaign) const {
  // Decision 0014: count in (hlc, origin) order, capped at the loop
  // budget; entries past the budget are void.
  const std::int64_t budget = campaign_budget(campaign);
  std::int64_t total = 0;
  for (const ReserveEntry& entry : reserve_entries_sorted(campaign)) {
    if (total + entry.attempts > budget) break;
    total += entry.attempts;
  }
  return total;
}

bool StateWriter::reservation_void(std::string_view campaign,
                                   std::string_view origin,
                                   log::Hlc hlc) const {
  const std::int64_t budget = campaign_budget(campaign);
  std::int64_t total = 0;
  for (const ReserveEntry& entry : reserve_entries_sorted(campaign)) {
    if (total + entry.attempts > budget) return true;  // past the cap: void
    total += entry.attempts;
    if (entry.origin == origin && entry.hlc == hlc) return false;
  }
  return true;  // not among the counted entries: void or unknown
}

std::optional<std::string> StateWriter::origin_prev_hash(
    std::string_view origin) const {
  std::shared_lock chain_lock(chain_mutex_);
  const auto found = origin_prev_hashes_.find(std::string(origin));
  if (found == origin_prev_hashes_.end()) return std::nullopt;
  return found->second;
}

std::map<std::string, std::int64_t> StateWriter::reserved_by_origin(
    std::string_view campaign) const {
  std::shared_lock lock(index_mutex_);
  std::map<std::string, std::int64_t> result;
  const std::string suffix = "|" + std::string(campaign);
  for (const auto& [key, total] : reserved_by_origin_campaign_) {
    if (key.size() >= suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0)
      result[key.substr(0, key.size() - suffix.size())] = total;
  }
  return result;
}

std::uint64_t StateWriter::next_seq() const { return next_seq_.load(); }
std::uint64_t StateWriter::next_batch() const { return next_batch_.load(); }

}  // namespace onion::state
