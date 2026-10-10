#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace onion::llm {
inline constexpr std::string_view component_name{"llm"};

struct LimitWindow {
  std::size_t calls{};
  std::size_t rate_limited{};
  std::size_t server_errors{};
  std::size_t timeouts{};
  double median_latency_s{};
};

class Limiter {
 public:
  Limiter(int starting_limit, int ceiling);

  void on_window(const LimitWindow& window);
  int limit() const;
  std::optional<double> low_load_baseline_s() const;

 private:
  int limit_;
  int ceiling_;
  bool hold_next_window_{false};
  std::optional<double> baseline_s_;
};
}
