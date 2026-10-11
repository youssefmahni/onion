#include "llm/llm.hpp"

#include <algorithm>

namespace onion::llm {
namespace {
// Spec constants (decision 0011).
constexpr double kRampFactor = 1.25;
constexpr double kPlateauProbeFactor = 1.1;
constexpr double kTailGuardFactor = 0.9;
constexpr double kGoodputRiseThreshold = 1.05;
constexpr double kTailGuardP95Ratio = 2.0;
constexpr double kEwmaAlpha = 0.5;
constexpr int kPlateauProbeInterval = 10;
constexpr int kBackoffHoldWindows = 1;
constexpr int kRevertHoldWindows = 2;

bool pressure(const LimitWindow& window) {
  return window.rate_limited != 0 || window.server_errors != 0 ||
      (window.calls != 0 && window.timeouts * 100 > window.calls * 5) ||
      (window.calls != 0 && window.connect_failures * 100 > window.calls * 2);
}

bool successful(const LimitWindow& window) {
  return !pressure(window) && window.completed_out_tokens != 0;
}
}  // namespace

Limiter::Limiter(int starting_limit, int ceiling, double window_length_s)
    : starting_limit_(std::max(1, std::min(starting_limit, ceiling))),
      limit_(starting_limit_), ceiling_(std::max(1, ceiling)),
      window_length_s_(window_length_s > 0.0 ? window_length_s : 30.0) {}

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
  // Idle windows never change state.
  if (window.calls == 0) return;

  const bool press = pressure(window);
  const bool success = successful(window);

  // 1. Hourly baseline probe (decision 0005, extended to record p95).
  if (probe_active_) {
    if (success) {
      baseline_s_ = window.median_latency_s;
      if (window.p95_latency_s > 0.0) baseline_p95_s_ = window.p95_latency_s;
    }
    limit_ = press ? std::max(1, pre_probe_limit_ / 2) : pre_probe_limit_;
    probe_active_ = false;
    if (press) {
      hold_windows_ = kBackoffHoldWindows;
      reset_ramp_state();
    }
    return;
  }

  // The first successful window at the starting limit anchors the low-load
  // baseline (median and p95), like decision 0005's initial anchor.
  if (success && !baseline_s_ && limit_ == starting_limit_) {
    baseline_s_ = window.median_latency_s;
    if (window.p95_latency_s > 0.0) baseline_p95_s_ = window.p95_latency_s;
    last_probe_started_s_ = 0;
  }

  // 2. Hard backoff: 429/503, >5% timeouts or >2% connect failures halve the
  // limit and hold one window.
  if (press) {
    limit_ = std::max(1, limit_ / 2);
    hold_windows_ = kBackoffHoldWindows;
    reset_ramp_state();
    return;
  }

  // From here the window must have completed tokens; windows with calls but
  // no completed tokens (all failed) change nothing.
  if (!success) return;

  // Update the goodput signal (completed output tokens per second, EWMA over
  // two windows).
  const double window_goodput =
      static_cast<double>(window.completed_out_tokens) / window_length_s_;
  goodput_ = goodput_valid_ ? kEwmaAlpha * window_goodput + (1.0 - kEwmaAlpha) * goodput_
                            : window_goodput;
  goodput_valid_ = true;

  // 3. Tail guard: p95 more than twice the low-load baseline p95 steps back
  // ×0.9 and enters the plateau state.  The step-back plays the role of a
  // revert, so the ramp cannot blindly re-increase into the danger zone; the
  // plateau probe tracks capacity recovery.  While the p95 stays above the
  // threshold the guard keeps stepping down every window; once it recovers,
  // the hold passes before the plateau counter starts.
  if (baseline_p95_s_ && window.p95_latency_s > kTailGuardP95Ratio * *baseline_p95_s_) {
    limit_ = std::max(1, static_cast<int>(limit_ * kTailGuardFactor));
    hold_windows_ = kBackoffHoldWindows;
    reset_ramp_state();
    enter_plateau_now();
    return;
  }

  // 4. Plateau probe: one window at ×1.1 after 10 plateau windows.
  if (plateau_probe_active_) {
    if (goodput_ >= kGoodputRiseThreshold * plateau_signal_) {
      // Capacity moved: adopt the probed limit; the ramp resumes with the
      // next window's unassessed increase.
      plateau_ = false;
      plateau_probe_active_ = false;
      plateau_windows_ = 0;
    } else {
      limit_ = plateau_limit_;
      plateau_probe_active_ = false;
      plateau_ = true;
      plateau_windows_ = 0;
    }
    return;
  }

  // Holds block proactive changes; backoff and the tail guard may still fire
  // (they are checked above).
  if (hold_windows_ > 0) {
    --hold_windows_;
    return;
  }

  if (plateau_) {
    if (limit_ >= ceiling_) return;  // nothing above the ceiling to probe
    ++plateau_windows_;
    if (plateau_windows_ >= kPlateauProbeInterval) {
      plateau_limit_ = limit_;
      limit_ = std::min(ceiling_, static_cast<int>(limit_ * kPlateauProbeFactor));
      plateau_probe_active_ = true;
      plateau_windows_ = 0;
    }
    return;
  }

  // 5/6. Ramp: increase by ×1.25 while goodput rises at least 5% per
  // increase; otherwise return to the previous limit and hold two windows.
  if (assessing_) {
    if (goodput_ >= kGoodputRiseThreshold * goodput_at_increase_) {
      const int before = limit_;
      limit_ = std::min(ceiling_, static_cast<int>(limit_ * kRampFactor));
      if (limit_ == before) {
        // Ceiling reached: hold there, no revert.
        assessing_ = false;
        enter_plateau_now();
      } else {
        goodput_at_increase_ = goodput_;
        pre_increase_limit_ = before;
      }
    } else {
      limit_ = pre_increase_limit_;
      assessing_ = false;
      hold_windows_ = kRevertHoldWindows;
      enter_plateau_now();
    }
  } else {
    goodput_at_increase_ = goodput_;
    pre_increase_limit_ = limit_;
    limit_ = std::min(ceiling_, static_cast<int>(limit_ * kRampFactor));
    if (limit_ == pre_increase_limit_) {
      enter_plateau_now();
    } else {
      assessing_ = true;
    }
  }
}

void Limiter::reset_ramp_state() {
  assessing_ = false;
  plateau_ = false;
  plateau_probe_active_ = false;
  plateau_windows_ = 0;
}

void Limiter::enter_plateau_now() {
  plateau_ = true;
  plateau_windows_ = 0;
  plateau_signal_ = goodput_;
}

int Limiter::limit() const { return limit_; }

std::optional<double> Limiter::low_load_baseline_s() const {
  return baseline_s_;
}

std::optional<double> Limiter::low_load_baseline_p95_s() const {
  return baseline_p95_s_;
}

double Limiter::goodput_signal() const { return goodput_; }
}  // namespace onion::llm
