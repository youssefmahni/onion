#include "metrics.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "llm/llm.hpp"
#include "store/store.hpp"

namespace onion::metrics {

namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS speed(ts INTEGER NOT NULL, inflight INTEGER NOT NULL,
                        limit_ INTEGER NOT NULL, answers_ps REAL NOT NULL,
                        out_tokens_ps REAL NOT NULL, p95_ms REAL NOT NULL,
                        errors INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS speed_ts ON speed(ts);
CREATE TABLE IF NOT EXISTS spend(ts_hour INTEGER NOT NULL, campaign TEXT NOT NULL,
                        peak_usd TEXT NOT NULL, offpeak_usd TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS spend_ts ON spend(ts_hour);
CREATE TABLE IF NOT EXISTS events(seq INTEGER PRIMARY KEY, ts INTEGER NOT NULL,
                        kind TEXT NOT NULL, detail TEXT NOT NULL);
)sql";

std::string money(double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.8f", value);
  std::string out(buffer);
  while (out.size() > 2 && out.back() == '0') out.pop_back();
  if (!out.empty() && out.back() == '.') out.pop_back();
  return out;
}

double parse_money(std::string_view text) {
  return std::atof(std::string(text).c_str());
}

std::int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

struct Store::Impl {
  Options options;
  store::Db db{options.path};
};

Store::Store(Options options) : impl_(new Impl{std::move(options)}) {}
Store::~Store() { stop(); }

bool Store::start(std::string* error) {
  if (!impl_->db.open(error)) return false;
  return impl_->db.exec(kSchema, error);
}

void Store::stop() { impl_->db.close(); }

bool Store::record_speed(SpeedSample sample, std::string* error) {
  auto statement = impl_->db.prepare(
      "INSERT INTO speed(ts, inflight, limit_, answers_ps, out_tokens_ps, "
      "p95_ms, errors) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);",
      error);
  if (!statement) return false;
  std::string bind_error;
  if (!statement->bind(1, sample.ts, &bind_error) ||
      !statement->bind(2, sample.inflight, &bind_error) ||
      !statement->bind(3, sample.limit, &bind_error) ||
      !statement->bind_double(4, sample.answers_ps, &bind_error) ||
      !statement->bind_double(5, sample.out_tokens_ps, &bind_error) ||
      !statement->bind_double(6, sample.p95_ms, &bind_error) ||
      !statement->bind(7, sample.errors, &bind_error)) {
    if (error) *error = "speed bind failed: " + bind_error;
    return false;
  }
  statement->step(&bind_error);
  if (!bind_error.empty()) {
    if (error) *error = "speed insert failed: " + bind_error;
    return false;
  }
  // Retention (D-5): 7 days at 30 s — prune older rows.
  auto prune = impl_->db.prepare("DELETE FROM speed WHERE ts < ?1;", error);
  if (!prune) return true;
  const std::int64_t cutoff = now_s() - impl_->options.speed_retention.count();
  prune->bind(1, cutoff, &bind_error);
  prune->step(&bind_error);
  return true;
}

std::vector<Store::SpeedSample> Store::speed_since(std::int64_t ts,
                                                   std::string* error) const {
  std::vector<SpeedSample> samples;
  auto statement = impl_->db.prepare(
      "SELECT ts, inflight, limit_, answers_ps, out_tokens_ps, p95_ms, "
      "errors FROM speed WHERE ts >= ?1 ORDER BY ts;",
      error);
  if (!statement) return samples;
  std::string bind_error;
  statement->bind(1, ts, &bind_error);
  std::string step_error;
  while (statement->step_row(&step_error))
    samples.push_back(SpeedSample{statement->integer(0),
                                  statement->integer(1),
                                  statement->integer(2),
                                  statement->real(3),
                                  statement->real(4),
                                  statement->real(5),
                                  statement->integer(6)});
  if (!step_error.empty() && error) *error = "speed query failed: " + step_error;
  return samples;
}

std::int64_t Store::speed_retention_seconds() const {
  return impl_->options.speed_retention.count();
}

bool Store::refresh_spend(const store::Db& source,
                          const std::optional<llm::PriceSchedule>& prices,
                          std::string* error) {
  // Recompute the hour buckets from the attempt rows: ts_hour = the
  // current wall-clock hour (the sampler attributes rowid-ordered rows at
  // ingestion time, so a crash only loses un-sampled hours, never corrupts
  // the retained ones).
  impl_->db.exec("DELETE FROM spend;", error);
  std::string query_error;
  auto attempts = source.prepare(
      "SELECT campaign, tokens_in, tokens_cached, tokens_out, cost FROM "
      "attempts;",
      &query_error);
  if (!attempts) {
    if (error) *error = "spend: attempts query failed: " + query_error;
    return false;
  }
  struct Bucket {
    double peak{};
    double offpeak{};
  };
  std::map<std::pair<std::int64_t, std::string>, Bucket> buckets;
  std::string step_error;
  const std::int64_t hour = now_s() / 3600 * 3600;
  while (attempts->step_row(&step_error)) {
    const std::string campaign = attempts->text(0);
    const std::int64_t tokens_in = attempts->integer(1);
    const std::int64_t tokens_cached = attempts->integer(2);
    const std::int64_t tokens_out = attempts->integer(3);
    const double total = parse_money(attempts->text(4));
    double peak = total;
    double offpeak = 0.0;
    if (prices) {
      const std::int64_t miss = std::max<std::int64_t>(0, tokens_in - tokens_cached);
      const std::int64_t hit = tokens_cached;
      const double peak_cost =
          miss * parse_money(prices->peak.input) / 1e6 +
          hit * parse_money(prices->peak.cached_input) / 1e6 +
          tokens_out * parse_money(prices->peak.output) / 1e6;
      const double off_cost =
          miss * parse_money(prices->offpeak.input) / 1e6 +
          hit * parse_money(prices->offpeak.cached_input) / 1e6 +
          tokens_out * parse_money(prices->offpeak.output) / 1e6;
      const double sum = peak_cost + off_cost;
      if (sum > 0) {
        peak = total * peak_cost / sum;
        offpeak = total * off_cost / sum;
      }
    }
    auto& bucket = buckets[{hour, campaign}];
    bucket.peak += peak;
    bucket.offpeak += offpeak;
  }
  if (!step_error.empty()) {
    if (error) *error = "spend: attempts scan failed: " + step_error;
    return false;
  }
  auto insert = impl_->db.prepare(
      "INSERT INTO spend(ts_hour, campaign, peak_usd, offpeak_usd) VALUES "
      "(?1, ?2, ?3, ?4);",
      error);
  if (!insert) return false;
  std::string bind_error;
  for (const auto& [key, bucket] : buckets) {
    if (!insert->bind(1, key.first, &bind_error) ||
        !insert->bind(2, key.second, &bind_error) ||
        !insert->bind(3, money(bucket.peak), &bind_error) ||
        !insert->bind(4, money(bucket.offpeak), &bind_error)) {
      if (error) *error = "spend bind failed: " + bind_error;
      return false;
    }
    insert->step(&bind_error);
    if (!bind_error.empty()) {
      if (error) *error = "spend insert failed: " + bind_error;
      return false;
    }
    insert->reset(&bind_error);
  }
  return true;
}

std::vector<Store::SpendHour> Store::spend_since(std::int64_t ts,
                                                 std::string* error) const {
  std::vector<SpendHour> hours;
  auto statement = impl_->db.prepare(
      "SELECT ts_hour, campaign, peak_usd, offpeak_usd FROM spend WHERE "
      "ts_hour >= ?1 ORDER BY ts_hour, campaign;",
      error);
  if (!statement) return hours;
  std::string bind_error;
  statement->bind(1, ts, &bind_error);
  std::string step_error;
  while (statement->step_row(&step_error))
    hours.push_back(SpendHour{statement->integer(0), statement->text(1),
                              statement->text(2), statement->text(3)});
  return hours;
}

bool Store::append_event(std::string kind, std::string detail,
                         std::string* error) {
  auto statement = impl_->db.prepare(
      "INSERT INTO events(ts, kind, detail) VALUES (?1, ?2, ?3);", error);
  if (!statement) return false;
  std::string bind_error;
  if (!statement->bind(1, now_s(), &bind_error) ||
      !statement->bind(2, std::move(kind), &bind_error) ||
      !statement->bind(3, std::move(detail), &bind_error)) {
    if (error) *error = "event bind failed: " + bind_error;
    return false;
  }
  statement->step(&bind_error);
  if (!bind_error.empty()) {
    if (error) *error = "event insert failed: " + bind_error;
    return false;
  }
  // Ring bound: keep the latest 10,000.
  impl_->db.exec(
      "DELETE FROM events WHERE seq <= (SELECT MAX(seq) FROM events) - "
      "10000;",
      nullptr);
  return true;
}

std::vector<Store::Event> Store::events_after(std::int64_t since, int limit,
                                              std::string* error) const {
  std::vector<Event> events;
  auto statement = impl_->db.prepare(
      "SELECT seq, ts, kind, detail FROM events WHERE seq > ?1 ORDER BY seq "
      "LIMIT ?2;",
      error);
  if (!statement) return events;
  std::string bind_error;
  if (!statement->bind(1, since, &bind_error) ||
      !statement->bind(2, static_cast<std::int64_t>(
                               std::max(1, std::min(limit, 200))),
                       &bind_error))
    return events;
  std::string step_error;
  while (statement->step_row(&step_error))
    events.push_back(Event{statement->integer(0), statement->integer(1),
                           statement->text(2), statement->text(3)});
  return events;
}

std::int64_t Store::latest_event_seq(std::string* error) const {
  auto statement =
      impl_->db.prepare("SELECT MAX(seq) FROM events;", error);
  if (!statement) return 0;
  std::string step_error;
  if (!statement->step_row(&step_error)) return 0;
  return statement->integer(0);
}

}  // namespace onion::metrics
