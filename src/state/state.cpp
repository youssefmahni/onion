#include "state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
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
      "layer=excluded.layer WHERE excluded.hlc > nodes.hlc;",
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
      "origin=excluded.origin WHERE excluded.hlc > settles.hlc;",
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
      "INSERT INTO attempts(path, n, score, passed, tokens_in, tokens_cached, "
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
  auto statement = db.prepare(
      "INSERT INTO critiques(path, score, issues, remove, add_, hlc, origin) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);",
      error);
  if (!statement) return;
  std::string bind_error;
  if (!statement->bind(1, payload.at("path").get<std::string>(), &bind_error) ||
      !statement->bind(2, payload.value("score", std::string{"0"}), &bind_error) ||
      !statement->bind(3, payload.value("issues", std::string{}), &bind_error) ||
      !statement->bind(4, payload.value("remove", std::string{}), &bind_error) ||
      !statement->bind(5, payload.value("add", std::string{}), &bind_error) ||
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
      "WHERE excluded.hlc > reviews.hlc;",
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
      "WHERE excluded.hlc > campaigns.hlc;",
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
  std::vector<std::filesystem::path> sealed_files;
  std::filesystem::path active_file;
  std::error_code ec;
  const std::filesystem::path dir = segments_->active_path().parent_path();
  if (std::filesystem::exists(dir))
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() == ".zst") sealed_files.push_back(entry.path());
      else active_file = entry.path();
    }
  std::sort(sealed_files.begin(), sealed_files.end());

  std::int64_t expected_first = 1;
  std::string expected_prev =
      options_.device_key ? log::genesis_prev(*options_.device_key)
                          : std::string{};
  std::vector<std::string> members{options_.origin};
  std::uint64_t expected_batch = 1;

  const auto process_line = [&](const std::string& line, std::string* line_error) {
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
    if (options_.device_key) {
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
    }
    if (batch.number != expected_batch && options_.device_key) {
      if (line_error) *line_error = "batch number gap";
      return false;
    }
    for (const log::Record& record : batch.records) {
      if (record.seq >= static_cast<std::int64_t>(next_seq_))
        next_seq_ = static_cast<std::uint64_t>(record.seq) + 1;
      fold(record, error);
    }
    applied_since_checkpoint_ += batch.records.size();
    next_batch_ = batch.number + 1;
    expected_batch = batch.number + 1;
    expected_prev = batch.hash;
    expected_first = static_cast<std::int64_t>(next_seq_.load());
    prev_hash_ = batch.hash;
    return true;
  };

  for (const auto& file : sealed_files) {
    const std::string content = decompress_segment(file);
    if (content.empty()) {
      if (error) *error = "cannot decompress sealed segment " + file.string();
      return;
    }
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      std::string line_error;
      if (!process_line(line, &line_error)) {
        if (error)
          *error = "sealed segment " + file.string() +
                   " failed verification: " + line_error +
                   " (corruption, not a torn tail)";
        return;
      }
    }
  }

  if (!active_file.empty()) {
    std::ifstream in(active_file, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    in.close();
    std::size_t good = 0;
    for (; good < lines.size(); ++good) {
      if (lines[good].empty()) {
        good++;
        continue;
      }
      std::string line_error;
      if (!process_line(lines[good], &line_error)) break;
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
  for (const store::Campaign& campaign : db_.campaigns(&error))
    campaigns_.push_back(CampaignInfo{campaign.id, campaign.spec, campaign.state});
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

void StateWriter::fold(const log::Record& record, std::string* error) {
  const std::string& type = record.type;
  if (type == "ADD_NODE") upsert_node(db_, record, options_.origin, error);
  else if (type == "SETTLE") upsert_settle(db_, record, options_.origin, error);
  else if (type == "ATTEMPT") upsert_attempt(db_, record, options_.origin, error);
  else if (type == "CRITIQUE") upsert_critique(db_, record, options_.origin, error);
  else if (type == "NEEDS_REVIEW") upsert_review(db_, record, options_.origin, error);
  else if (type == "CAMPAIGN") upsert_campaign(db_, record, options_.origin, error);
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
    // stop is final, in the index as in the store.
    auto it = std::find_if(campaigns_.begin(), campaigns_.end(),
                           [&](const CampaignInfo& c) { return c.id == id; });
    if (it != campaigns_.end() && it->state == "stopped") state = "stopped";
    campaigns_.erase(std::remove_if(campaigns_.begin(), campaigns_.end(),
                                    [&](const CampaignInfo& c) { return c.id == id; }),
                     campaigns_.end());
    campaigns_.push_back(CampaignInfo{id, payload.at("spec").dump(), state});
  }
}

bool StateWriter::append_batch(std::vector<log::Record> records,
                               std::string* error) {
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
  if (options_.crash_hook) options_.crash_hook(1);  // batch durable, store stale
  for (const log::Record& record : records) fold(record, error);
  if (error && !error->empty()) return false;
  if (options_.crash_hook) options_.crash_hook(2);  // fold done
  return true;
}

bool StateWriter::flush_pending_attempts(std::string* error) {
  if (pending_attempts_.empty()) return true;
  std::vector<log::Record> records;
  records.swap(pending_attempts_);
  last_flush_ = std::chrono::steady_clock::now();
  return append_batch(std::move(records), error);
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
  // Release the database and the segment handle so the data directory can
  // be removed (Windows keeps open files locked).
  if (segments_) segments_->close();
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

std::uint64_t StateWriter::next_seq() const { return next_seq_.load(); }
std::uint64_t StateWriter::next_batch() const { return next_batch_.load(); }

}  // namespace onion::state
