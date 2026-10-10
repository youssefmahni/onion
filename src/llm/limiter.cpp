#include "llm/llm.hpp"

#include <algorithm>

namespace onion::llm {
Limiter::Limiter(int starting_limit, int ceiling)
    : limit_(std::max(1, std::min(starting_limit, ceiling))), ceiling_(std::max(1, ceiling)) {}

void Limiter::on_window(const LimitWindow& window) {
  const bool transport_pressure = window.rate_limited != 0 || window.server_errors != 0 ||
      (window.calls != 0 && window.timeouts * 100 > window.calls * 5);
  if (transport_pressure) {
    limit_ = std::max(1, limit_ / 2);
    hold_next_window_ = true;
    return;
  }

  if (window.median_latency_s > 0.0 && !baseline_s_) baseline_s_ = window.median_latency_s;
  if (hold_next_window_) {
    hold_next_window_ = false;
    return;
  }
  if (baseline_s_ && window.median_latency_s > 0.0 &&
      window.median_latency_s <= *baseline_s_ * 1.5) {
    limit_ = std::min(ceiling_, limit_ + 4);
  }
}

int Limiter::limit() const { return limit_; }

std::optional<double> Limiter::low_load_baseline_s() const { return baseline_s_; }
}  // namespace onion::llm
