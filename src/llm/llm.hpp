#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace onion::llm {
inline constexpr std::string_view component_name{"llm"};

struct LimitWindow {
  std::uint64_t ended_at_s{};
  std::size_t calls{};
  std::size_t rate_limited{};
  std::size_t server_errors{};
  std::size_t timeouts{};
  std::size_t connect_failures{};
  std::uint64_t completed_out_tokens{};
  double median_latency_s{};
  double p95_latency_s{};
};

// Limiter v2 (decision 0011): goodput-based control.  The limit is evaluated
// once per window; window_length_s is injectable for tests (default 30).
class Limiter {
 public:
  Limiter(int starting_limit, int ceiling, double window_length_s = 30.0);

  // Returns the limit for this window. Once an hour it begins a probe window
  // at a reduced limit (decision 0005, extended to record p95).
  int begin_window(std::uint64_t started_at_s);
  void on_window(const LimitWindow& window);
  int limit() const;
  std::optional<double> low_load_baseline_s() const;
  std::optional<double> low_load_baseline_p95_s() const;
  double goodput_signal() const;

 private:
  void enter_plateau_now();
  int starting_limit_;
  int limit_;
  int ceiling_;
  double window_length_s_;
  int hold_windows_{0};
  bool probe_active_{false};
  int pre_probe_limit_{0};
  std::optional<std::uint64_t> last_probe_started_s_;
  std::optional<double> baseline_s_;
  std::optional<double> baseline_p95_s_;
  double goodput_{0};
  bool goodput_valid_{false};
  bool assessing_{false};
  double g_at_increase_{0};
  int pre_increase_limit_{0};
  bool plateau_{false};
  int plateau_windows_{0};
  bool plateau_probe_active_{false};
  int plateau_limit_{0};
  double plateau_signal_{0};
};

struct Request {
  std::string model;
  std::string system;
  std::string user;
  int max_tokens{6000};
  double temperature{};
};

enum class Kind { Ok, Transport, Truncated, RateLimited, ServerError, Timeout, ClientError, AuthError, BalanceError, Malformed };

struct Result {
  Kind kind{Kind::Transport};
  int http_status{};
  std::string content;
  std::string finish_reason;
  std::uint64_t in_hit{};
  std::uint64_t in_miss{};
  std::uint64_t out{};
  std::string cost_usd{"0"};
  double latency_s{};
  long retry_after_s{};
};

struct Prices {
  std::string input;
  std::string cached_input;
  std::string output;
};

struct PriceSchedule {
  Prices peak;
  Prices offpeak;
};

std::string meter_cost(std::uint64_t cache_hit, std::uint64_t cache_miss,
                       std::uint64_t output, const PriceSchedule& prices,
                       bool request_stayed_in_one_window, bool offpeak_window);

struct Reservation { std::uint64_t id{}; std::string reserved_usd; };

class Budget {
 public:
  explicit Budget(std::string limit_usd);
  std::optional<Reservation> reserve(std::string usd);
  void settle(const Reservation& reservation, std::string actual_usd);
  std::string spent_usd() const;
  std::string reserved_usd() const;
  std::string remaining_usd() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

class CircuitBreaker {
 public:
  void on_result(Kind kind);
  bool open() const;
  int consecutive_transport_failures() const;
  std::string reason() const;

 private:
  int failures_{};
  int credentials_{};
  std::string reason_;
};

class Client {
 public:
  struct Options {
    std::string base_url;
    std::string api_key;
    int timeout_s{120};
    int connect_timeout_s{10};
    int worker_threads{1};
    std::size_t max_in_flight{1000};
  };
  explicit Client(Options options);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  void submit(Request request, std::function<void(Result)> done);
  std::size_t in_flight() const;
  std::size_t peak_in_flight() const;
  std::size_t peak_active_requests() const;
  std::size_t connections_opened() const;
  long negotiated_http_version() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool curl_supports_http2();
std::string curl_version();
// Blocking GET used by the developer CLI (free /models check).
std::string http_get(const std::string& url, const std::string& api_key,
                    long& http_status, long& http_version);
}
