#include "llm/llm.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
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
  std::mutex mutex; std::condition_variable done; int completed = 0; int failed = 0; int transport = 0;
  const auto started = std::chrono::steady_clock::now();
  for (int index = 0; index < requests; ++index) {
    client.submit({"fake", "fixed instruction", "bench request", 32, 0.0},
      [&](onion::llm::Result result) { std::lock_guard<std::mutex> lock(mutex); breaker.on_result(result.kind); if (result.kind != onion::llm::Kind::Ok) ++failed; if (result.kind == onion::llm::Kind::Transport) ++transport; if (++completed == requests) done.notify_one(); });
  }
  std::unique_lock<std::mutex> lock(mutex);
  done.wait(lock, [&] { return completed == requests; });
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "requests=" << requests << " failed=" << failed << " elapsed_s=" << elapsed
            << " throughput=" << requests / elapsed << " peak_in_flight=" << client.peak_in_flight() << " threads=1\n";
  std::cout << "transport=" << transport << "\n";
  std::cout << "breaker_open=" << (breaker.open() ? "true" : "false") << "\n";
  return failed == 0 ? 0 : 1;
}
