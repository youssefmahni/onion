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

  if (window.median_latency_s > 0.0) {
    healthy_latencies_.push_back({window.ended_at_s, window.median_latency_s});
    while (!healthy_latencies_.empty() &&
           healthy_latencies_.front().ended_at_s + 3600 < window.ended_at_s) {
      healthy_latencies_.pop_front();
    }
  }
  if (hold_next_window_) {
    hold_next_window_ = false;
    return;
  }
  const auto baseline = low_load_baseline_s();
  if (baseline && window.median_latency_s > 0.0 &&
      window.median_latency_s <= *baseline * 1.5) {
    limit_ = std::min(ceiling_, limit_ + 4);
  }
}

int Limiter::limit() const { return limit_; }

std::optional<double> Limiter::low_load_baseline_s() const {
  if (healthy_latencies_.empty()) return std::nullopt;
  double lowest = healthy_latencies_.front().median_latency_s;
  for (const auto& sample : healthy_latencies_) {
    lowest = std::min(lowest, sample.median_latency_s);
  }
  return lowest;
}
}  // namespace onion::llm
