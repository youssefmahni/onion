#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace onion::store {
class Db;
}
namespace onion::llm {
struct PriceSchedule;
}

namespace onion::metrics {
inline constexpr std::string_view component_name{"metrics"};

// metrics.db (plan §6.3): local telemetry beside onion.db. Speed samples
// every 30 s (retention 7 days, decision D-5), spend hour buckets derived
// from attempt rows, and the bounded activity-event ring (10,000) that
// feeds Recent activity and the stream. Purely local: never part of the
// signed log, never synced.
class Store {
 public:
  struct Options {
    std::filesystem::path path;  // <data_dir>/metrics.db
    std::chrono::seconds speed_retention{7 * 24 * 3600};
  };

  struct Event {
    std::int64_t seq{};
    std::int64_t ts{};
    std::string kind;
    std::string detail;
  };

  struct SpeedSample {
    std::int64_t ts{};
    std::int64_t inflight{};
    std::int64_t limit{};
    double answers_ps{};
    double out_tokens_ps{};
    double p95_ms{};
    std::int64_t errors{};
  };

  struct SpendHour {
    std::int64_t ts_hour{};
    std::string campaign;
    std::string peak_usd;
    std::string offpeak_usd;
  };

  explicit Store(Options options);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  bool start(std::string* error);
  void stop();

  // Appends one speed sample and prunes beyond the retention window.
  bool record_speed(SpeedSample sample, std::string* error);
  std::vector<SpeedSample> speed_since(std::int64_t ts,
                                       std::string* error) const;
  std::int64_t speed_retention_seconds() const;

  // Replaces the spend hour buckets from the source's attempt rows
  // (rowid-ordered: the sampler attributes new rows to the current hour).
  // With a price schedule, the cost splits into peak/off-peak by token
  // mix (hit/cached vs miss) exactly like the projection (sched.cpp);
  // without one, everything counts as peak.
  bool refresh_spend(const store::Db& source,
                     const std::optional<llm::PriceSchedule>& prices,
                     std::string* error);
  std::vector<SpendHour> spend_since(std::int64_t ts,
                                     std::string* error) const;

  // Activity events: seq auto-increments; the ring keeps the latest 10,000.
  bool append_event(std::string kind, std::string detail, std::string* error);
  std::vector<Event> events_after(std::int64_t since, int limit,
                                  std::string* error) const;
  std::int64_t latest_event_seq(std::string* error) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onion::metrics
