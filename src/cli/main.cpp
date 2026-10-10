#include "llm/llm.hpp"

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

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "llm") {
    std::cerr << "usage: data_depot llm bench --url <fake-server-url> --requests N\n";
    return 2;
  }
  std::string url;
  int requests = 0;
  int timeout_s = 120;
  for (int index = 2; index < argc; ++index) {
    const std::string arg(argv[index]);
    if (arg == "bench") continue;
    if (arg == "--url" && index + 1 < argc) url = argv[++index];
    else if (arg == "--requests" && index + 1 < argc) requests = std::atoi(argv[++index]);
    else if (arg == "--timeout" && index + 1 < argc) timeout_s = std::atoi(argv[++index]);
  }
  if (url.empty() || requests <= 0) {
    std::cerr << "bench requires --url and --requests\n";
    return 2;
  }
  onion::llm::Client client({url, {}, timeout_s, 1});
  onion::llm::CircuitBreaker breaker;
  onion::llm::Budget budget("10");
  std::mutex mutex; std::condition_variable done; int completed = 0; int failed = 0; int transport = 0; std::vector<double> latencies;
  const auto started = std::chrono::steady_clock::now();
  int reserved = 0;
  for (int index = 0; index < requests; ++index) {
    auto reservation = budget.reserve("0.01");
    if (!reservation) break;
    ++reserved;
    client.submit({"fake", "fixed instruction", "bench request", 32, 0.0},
      [&, reservation = *reservation](onion::llm::Result result) { budget.settle(reservation, result.kind == onion::llm::Kind::Ok ? "0.001" : "0"); std::lock_guard<std::mutex> lock(mutex); latencies.push_back(result.latency_s); breaker.on_result(result.kind); if (result.kind != onion::llm::Kind::Ok) ++failed; if (result.kind == onion::llm::Kind::Transport) ++transport; if (++completed == reserved) done.notify_one(); });
  }
  std::unique_lock<std::mutex> lock(mutex);
  done.wait(lock, [&] { return completed == reserved; });
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::sort(latencies.begin(), latencies.end());
  const auto percentile = [&](double fraction) { return latencies[static_cast<std::size_t>((latencies.size() - 1) * fraction)]; };
  std::cout << "requests=" << reserved << " failed=" << failed << " elapsed_s=" << elapsed
            << " throughput=" << requests / elapsed << " peak_in_flight=" << client.peak_in_flight() << " threads=1\n";
  std::cout << "latency_p50_s=" << percentile(.50) << " latency_p95_s=" << percentile(.95) << " latency_p99_s=" << percentile(.99) << "\n";
  std::cout << "connections_opened=" << client.connections_opened() << " http_version=" << client.negotiated_http_version() << "\n";
  std::cout << "curl_version=" << onion::llm::curl_version() << " curl_http2=" << (onion::llm::curl_supports_http2() ? "true" : "false") << " process_threads=" << process_threads() << "\n";
  std::cout << "budget_reserved=" << budget.reserved_usd() << " budget_spent=" << budget.spent_usd() << " budget_remaining=" << budget.remaining_usd() << "\n";
  std::cout << "transport=" << transport << "\n";
  std::cout << "breaker_open=" << (breaker.open() ? "true" : "false") << "\n";
  return failed == 0 ? 0 : 1;
}
