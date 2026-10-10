#include "llm/llm.hpp"

#include <algorithm>

namespace onion::llm {
Limiter::Limiter(int starting_limit, int ceiling)
    : starting_limit_(std::max(1, std::min(starting_limit, ceiling))),
      limit_(starting_limit_), ceiling_(std::max(1, ceiling)) {}

int Limiter::begin_window(std::uint64_t started_at_s) {
  if (!probe_active_ && baseline_s_ &&
      (!last_probe_started_s_ || started_at_s >= *last_probe_started_s_ + 3600)) {
    probe_active_ = true;
    pre_probe_limit_ = limit_;
    last_probe_started_s_ = started_at_s;
    return std::max(starting_limit_, limit_ / 4);
  }
  return limit_;
}

void Limiter::on_window(const LimitWindow& window) {
  const bool transport_pressure = window.rate_limited != 0 || window.server_errors != 0 ||
      (window.calls != 0 && window.timeouts * 100 > window.calls * 5);
  const bool successful = !transport_pressure && window.median_latency_s > 0.0;

  if (probe_active_) {
    if (successful) baseline_s_ = window.median_latency_s;
    limit_ = transport_pressure ? std::max(1, pre_probe_limit_ / 2) : pre_probe_limit_;
    probe_active_ = false;
    if (transport_pressure) hold_next_window_ = true;
    return;
  }

  if (successful && !baseline_s_ && limit_ == starting_limit_) baseline_s_ = window.median_latency_s;
  if (transport_pressure) {
    limit_ = std::max(1, limit_ / 2);
    hold_next_window_ = true;
    return;
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
  return baseline_s_;
}
}  // namespace onion::llm
