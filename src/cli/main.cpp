#include "llm/llm.hpp"
#include "platform/platform.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <vector>
#ifdef __linux__
#include <fstream>
#endif

namespace {
int process_threads() {
#ifdef __linux__
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key == "Threads:") { int value = 0; status >> value; return value; }
    status.ignore(4096, '\n');
  }
#endif
  return 0;
}
}
#include <string>

namespace {
int limiter_trace(int argc, char** argv) {
  std::string scenario;
  for (int index = 3; index < argc; ++index) {
    if (std::string(argv[index]) == "--scenario" && index + 1 < argc) scenario = argv[++index];
  }
  if (scenario != "healthy" && scenario != "rate_limited" && scenario != "slow_under_load") {
    std::cerr << "limiter-trace requires --scenario healthy|rate_limited|slow_under_load\n";
    return 2;
  }
  onion::llm::Limiter limiter(16, 2000);
  std::vector<std::uint64_t> starts{0, 30, 60, 90, 120, 150, 3600, 3630, 3660};
  for (const auto started_at_s : starts) {
    const int used = limiter.begin_window(started_at_s);
    onion::llm::LimitWindow window;
    window.ended_at_s = started_at_s + 30;
    window.calls = static_cast<std::size_t>(used);
    window.median_latency_s = 0.1;
    if (scenario == "rate_limited" && started_at_s == 60) window.rate_limited = 1;
    if (scenario == "slow_under_load" && started_at_s >= 30 && started_at_s < 3600) window.median_latency_s = 0.2;
    limiter.on_window(window);
    std::cout << "window_s=" << started_at_s << " used_limit=" << used
              << " rate_limited=" << window.rate_limited
              << " median_latency_s=" << window.median_latency_s
              << " next_limit=" << limiter.limit() << "\n";
  }
  return 0;
}
}

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "llm") {
    std::cerr << "usage: data_depot llm bench --url <fake-server-url> --requests N\n";
    return 2;
  }
  if (argc >= 3 && std::string(argv[2]) == "limiter-trace") return limiter_trace(argc, argv);
  std::string url;
  int requests = 0;
  int timeout_s = 120;
  int limit = 1000;
  std::string budget_limit = "10";
  for (int index = 2; index < argc; ++index) {
    const std::string arg(argv[index]);
    if (arg == "bench") continue;
    if (arg == "--url" && index + 1 < argc) url = argv[++index];
    else if (arg == "--requests" && index + 1 < argc) requests = std::atoi(argv[++index]);
    else if (arg == "--timeout" && index + 1 < argc) timeout_s = std::atoi(argv[++index]);
    else if (arg == "--limit" && index + 1 < argc) limit = std::atoi(argv[++index]);
    else if (arg == "--budget" && index + 1 < argc) budget_limit = argv[++index];
  }
  if (url.empty() || requests <= 0) {
    std::cerr << "bench requires --url and --requests\n";
    return 2;
  }
  if (limit <= 0) {
    std::cerr << "bench requires a positive --limit\n";
    return 2;
  }
  onion::llm::Client client({url, {}, timeout_s, 1, static_cast<std::size_t>(limit)});
  onion::llm::CircuitBreaker breaker;
  onion::llm::Budget budget(budget_limit);
  std::mutex mutex; std::condition_variable done; int completed = 0; int failed = 0; int transport = 0; int timeouts = 0; std::vector<double> latencies;
  const auto started = std::chrono::steady_clock::now();
  std::vector<onion::llm::Reservation> reservations;
  reservations.reserve(static_cast<std::size_t>(requests));
  for (int index = 0; index < requests; ++index) {
    auto reservation = budget.reserve("0.01");
    if (!reservation) break;
    reservations.push_back(std::move(*reservation));
  }
  const int accepted = static_cast<int>(reservations.size());
  const int refused = requests - accepted;
  for (const auto& reservation : reservations) {
    client.submit({"fake", "fixed instruction", "bench request", 32, 0.0},
      [&, reservation, accepted](onion::llm::Result result) {
        budget.settle(reservation, result.kind == onion::llm::Kind::Timeout ? reservation.reserved_usd : result.kind == onion::llm::Kind::Ok ? "0.001" : "0");
        std::lock_guard<std::mutex> lock(mutex);
        latencies.push_back(result.latency_s); breaker.on_result(result.kind);
        if (result.kind != onion::llm::Kind::Ok) ++failed;
        if (result.kind == onion::llm::Kind::Transport) ++transport;
        if (result.kind == onion::llm::Kind::Timeout) ++timeouts;
        if (++completed == accepted) done.notify_one();
      });
  }
  std::unique_lock<std::mutex> lock(mutex);
  done.wait(lock, [&] { return completed == accepted; });
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::sort(latencies.begin(), latencies.end());
  const auto percentile = [&](double fraction) { return latencies[static_cast<std::size_t>((latencies.size() - 1) * fraction)]; };
  std::cout << "requests_started=" << accepted << " requests_refused_budget=" << refused
            << " failed=" << failed << " elapsed_s=" << elapsed
            << " throughput=" << accepted / elapsed << " peak_queued_or_active=" << client.peak_in_flight()
            << " peak_active=" << client.peak_active_requests() << " configured_limit=" << limit << "\n";
  std::cout << "latency_p50_s=" << percentile(.50) << " latency_p95_s=" << percentile(.95) << " latency_p99_s=" << percentile(.99) << "\n";
  std::cout << "connections_opened=" << client.connections_opened() << " http_version=" << client.negotiated_http_version() << "\n";
  std::cout << "curl_version=" << onion::llm::curl_version() << " curl_http2=" << (onion::llm::curl_supports_http2() ? "true" : "false") << " process_threads=" << process_threads() << " fd_request_ceiling=" << onion::platform::outbound_request_ceiling() << "\n";
  std::cout << "budget_reserved=" << budget.reserved_usd() << " budget_spent=" << budget.spent_usd() << " budget_remaining=" << budget.remaining_usd() << "\n";
  std::cout << "transport=" << transport << "\n";
  std::cout << "timeouts_uncertain_billed=" << timeouts << "\n";
  std::cout << "breaker_open=" << (breaker.open() ? "true" : "false")
            << " breaker_reason=" << breaker.reason() << "\n";
  return failed == 0 ? 0 : 1;
}
