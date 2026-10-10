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
  double median_latency_s{};
};

class Limiter {
 public:
  Limiter(int starting_limit, int ceiling);

  // Returns the limit for this 30-second window. Once an hour it begins a
  // probe window at a reduced limit; callers pass the result back in
  // LimitWindow::limit_used.
  int begin_window(std::uint64_t started_at_s);
  void on_window(const LimitWindow& window);
  int limit() const;
  std::optional<double> low_load_baseline_s() const;

 private:
  int starting_limit_;
  int limit_;
  int ceiling_;
  bool hold_next_window_{false};
  bool probe_active_{false};
  int pre_probe_limit_{0};
  std::optional<std::uint64_t> last_probe_started_s_;
  std::optional<double> baseline_s_;
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
}
