#include "api/control_json.hpp"

#include <chrono>
#include <ctime>
#include <map>

#include "metrics/metrics.hpp"
#include "state/state.hpp"
#include "store/store.hpp"

namespace onion::api::control_json {

namespace {

std::int64_t now_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t range_seconds(std::string_view range) {
  if (range == "1h") return 3600;
  if (range == "7d") return 7 * 24 * 3600;
  return 24 * 3600;  // default 24h
}

}  // namespace

nlohmann::json summary(const state::StateWriter& writer,
                       const metrics::Store* metrics,
                       const SummaryProbes& probes) {
  std::string error;
  const store::Db& read = writer.read_db();
  nlohmann::json result;

  result["inflight"] = probes.inflight;
  result["limit"] = probes.limit;
  result["budget_usd"] = probes.device_budget_usd;

  // Today's spend: the metrics hour buckets since local midnight.
  if (metrics) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    const std::int64_t midnight = static_cast<std::int64_t>(std::mktime(&local));
    double today = 0;
    for (const auto& hour : metrics->spend_since(midnight, &error)) {
      today += std::atof(hour.peak_usd.c_str());
      today += std::atof(hour.offpeak_usd.c_str());
    }
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.8f", today);
    std::string text(buffer);
    while (text.size() > 2 && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    result["spend_today_usd"] = text;
  } else {
    result["spend_today_usd"] = "0";
  }

  // Counts from the §6.1 counters (with the on-the-fly fallback for
  // legacy copies, like tree/stats).
  auto levels = read.level_counts(&error);
  std::int64_t nodes = 0;
  for (const auto& [layer, count] : levels) nodes += count;
  if (nodes == 0) {
    auto statement = read.prepare(
        "SELECT COUNT(*) FROM nodes WHERE tombstone = 0;", &error);
    if (statement) {
      std::string step_error;
      if (statement->step_row(&step_error)) nodes = statement->integer(0);
    }
  }
  auto statuses = read.status_counts(&error);
  std::int64_t settled = 0, leaves = 0;
  for (const auto& [status, count] : statuses) {
    settled += count;
    if (status != "children") leaves += count;
  }
  std::int64_t atomic = 0;
  {
    auto statement =
        read.prepare("SELECT COUNT(*) FROM settles WHERE atomic = 1;", &error);
    if (statement) {
      std::string step_error;
      if (statement->step_row(&step_error)) atomic = statement->integer(0);
    }
  }
  std::int64_t review = 0;
  {
    auto statement = read.prepare(
        "SELECT COUNT(*) FROM reviews WHERE state IS NULL OR state != "
        "'decided';",
        &error);
    if (statement) {
      std::string step_error;
      if (statement->step_row(&step_error)) review = statement->integer(0);
    }
  }
  result["counts"] = {{"nodes", nodes},
                      {"settled", settled},
                      {"leaves", leaves},
                      {"atomic", atomic},
                      {"review", review}};

  // Campaign summaries (the details screen arrives in D3).
  nlohmann::json campaigns = nlohmann::json::array();
  for (const auto& campaign : writer.campaigns())
    campaigns.push_back({{"id", campaign.id},
                         {"state", campaign.state},
                         {"reason", campaign.reason},
                         {"reserved", writer.reserved_total(campaign.id)}});
  result["campaigns"] = std::move(campaigns);

  // Fleet members as the local node sees them.
  nlohmann::json members = nlohmann::json::array();
  for (const auto& row : state::StateWriter::members_table_on(read))
    members.push_back(
        {{"device_id", row.device_id}, {"state", row.state}});
  result["members"] = std::move(members);

  result["sync"] = {{"rounds_completed", probes.sync_rounds_completed},
                    {"seconds_since_last_round",
                     probes.seconds_since_last_round}};
  result["viewcache_ready"] = probes.viewcache_ready;
  return result;
}

nlohmann::json speed_series(const metrics::Store& metrics,
                            std::string_view range,
                            std::size_t max_points) {
  std::string error;
  const std::int64_t since = now_s() - range_seconds(range);
  const auto samples = metrics.speed_since(since, &error);
  nlohmann::json result;
  if (!error.empty()) {
    result["error"] = error;
    return result;
  }
  nlohmann::json series = nlohmann::json::array();
  if (samples.empty()) {
    result["series"] = std::move(series);
    result["range"] = range;
    return result;
  }
  // Bucketed downsample: at most max_points buckets of means.
  const std::size_t bucket_width =
      std::max<std::size_t>(1, (samples.size() + max_points - 1) / max_points);
  struct Mean {
    double answers = 0, out = 0, p95 = 0;
    std::int64_t errors = 0;
    std::int64_t ts = 0, inflight = 0, limit = 0;
    std::size_t n = 0;
  };
  Mean mean;
  for (const auto& sample : samples) {
    mean.answers += sample.answers_ps;
    mean.out += sample.out_tokens_ps;
    mean.p95 += sample.p95_ms;
    mean.errors += sample.errors;
    mean.inflight += sample.inflight;
    mean.limit += sample.limit;
    mean.ts = sample.ts;
    ++mean.n;
    if (mean.n == bucket_width) {
      series.push_back({{"ts", mean.ts},
                        {"inflight", mean.inflight / static_cast<double>(mean.n)},
                        {"limit", mean.limit / static_cast<double>(mean.n)},
                        {"answers_ps", mean.answers / static_cast<double>(mean.n)},
                        {"out_tokens_ps", mean.out / static_cast<double>(mean.n)},
                        {"p95_ms", mean.p95 / static_cast<double>(mean.n)},
                        {"errors", mean.errors}});
      mean = Mean{};
    }
  }
  if (mean.n > 0)
    series.push_back({{"ts", mean.ts},
                      {"inflight", mean.inflight / static_cast<double>(mean.n)},
                      {"limit", mean.limit / static_cast<double>(mean.n)},
                      {"answers_ps", mean.answers / static_cast<double>(mean.n)},
                      {"out_tokens_ps", mean.out / static_cast<double>(mean.n)},
                      {"p95_ms", mean.p95 / static_cast<double>(mean.n)},
                      {"errors", mean.errors}});
  result["series"] = std::move(series);
  result["range"] = range;
  return result;
}

nlohmann::json spend_series(const metrics::Store& metrics,
                            std::string_view range) {
  std::string error;
  const std::int64_t since = now_s() - range_seconds(range);
  const auto hours = metrics.spend_since(since, &error);
  nlohmann::json result;
  if (!error.empty()) {
    result["error"] = error;
    return result;
  }
  // Aggregate campaigns per hour (the chart shows one peak/off-peak pair
  // per hour; per-campaign detail lives in the campaigns screen).
  std::map<std::int64_t, std::pair<double, double>> by_hour;
  for (const auto& hour : hours)
    by_hour[hour.ts_hour].first += std::atof(hour.peak_usd.c_str()),
        by_hour[hour.ts_hour].second += std::atof(hour.offpeak_usd.c_str());
  nlohmann::json series = nlohmann::json::array();
  char buffer[48];
  for (const auto& [ts_hour, amounts] : by_hour) {
    const auto fmt = [&](double value) {
      std::snprintf(buffer, sizeof(buffer), "%.8f", value);
      std::string text(buffer);
      while (text.size() > 2 && text.back() == '0') text.pop_back();
      if (!text.empty() && text.back() == '.') text.pop_back();
      return text;
    };
    series.push_back({{"ts_hour", ts_hour},
                      {"peak_usd", fmt(amounts.first)},
                      {"offpeak_usd", fmt(amounts.second)}});
  }
  result["hours"] = std::move(series);
  result["range"] = range;
  return result;
}

nlohmann::json events_since(const metrics::Store& metrics,
                            std::int64_t since) {
  std::string error;
  const auto events = metrics.events_after(since, 200, &error);
  nlohmann::json result;
  if (!error.empty()) {
    result["error"] = error;
    return result;
  }
  nlohmann::json items = nlohmann::json::array();
  for (const auto& event : events)
    items.push_back({{"seq", event.seq},
                     {"ts", event.ts},
                     {"kind", event.kind},
                     {"detail", event.detail}});
  result["items"] = std::move(items);
  return result;
}

}  // namespace onion::api::control_json
