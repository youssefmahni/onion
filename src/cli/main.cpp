#include "llm/llm.hpp"
#include "platform/platform.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <thread>
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
  bool csv = false;
  for (int index = 3; index < argc; ++index) {
    if (std::string(argv[index]) == "--scenario" && index + 1 < argc) scenario = argv[++index];
    else if (std::string(argv[index]) == "--csv") csv = true;
  }
  if (scenario != "healthy" && scenario != "rate_limited" && scenario != "slow_under_load") {
    std::cerr << "limiter-trace requires --scenario healthy|rate_limited|slow_under_load\n";
    return 2;
  }
  onion::llm::Limiter limiter(16, 2000, 30.0);
  // Compressed trace: a handful of real 30-second windows, then the hourly
  // probe at t=3600 and its aftermath.  The limit is never reset between
  // samples, so every row is the exact limiter state for that window.
  // The modeled provider is utilization-bound: goodput scales with the limit
  // used, except that `slow_under_load` caps the goodput once the limit
  // passes the provider's plateau (16 x 40 tokens per window).
  std::vector<std::uint64_t> starts{0, 30, 60, 90, 120, 150, 3600, 3630, 3660};
  if (csv)
    std::cout << "window_start_s,window_end_s,limit_used,probe,rate_limited,"
                 "median_latency_s,p95_latency_s,out_tokens,limit_after\n";
  for (const auto started_at_s : starts) {
    const int before = limiter.limit();
    const int used = limiter.begin_window(started_at_s);
    onion::llm::LimitWindow window;
    window.ended_at_s = started_at_s + 30;
    window.calls = static_cast<std::size_t>(used);
    std::uint64_t out = static_cast<std::uint64_t>(used) * 40;
    if (scenario == "slow_under_load" && started_at_s >= 30 && started_at_s < 3600)
      out = 16 * 40;
    window.completed_out_tokens = out;
    window.median_latency_s = 0.1;
    window.p95_latency_s = 1.0;
    if (scenario == "rate_limited" && started_at_s == 60) window.rate_limited = 1;
    limiter.on_window(window);
    if (csv) {
      std::cout << started_at_s << "," << started_at_s + 30 << "," << used << ","
                << (used != before ? "1" : "0") << "," << window.rate_limited << ","
                << window.median_latency_s << "," << window.p95_latency_s << ","
                << out << "," << limiter.limit() << "\n";
    } else {
      std::cout << "window_s=" << started_at_s << " used_limit=" << used
                << " rate_limited=" << window.rate_limited
                << " median_latency_s=" << window.median_latency_s
                << " p95_latency_s=" << window.p95_latency_s
                << " out_tokens=" << out
                << " next_limit=" << limiter.limit() << "\n";
    }
  }
  return 0;
}
}

namespace {
int llm_models(int argc, char** argv) {
  std::string url;
  for (int index = 3; index < argc; ++index) {
    if (std::string(argv[index]) == "--url" && index + 1 < argc) url = argv[++index];
  }
  if (url.empty()) { std::cerr << "models requires --url <base-url>\n"; return 2; }
  const char* key = std::getenv("DEEPSEEK_API_KEY");
  if (!key || !*key) { std::cout << "key=missing\n"; return 2; }
  long status = 0; long version = 0;
  const std::string body = onion::llm::http_get(url + "/models", std::string(key), status, version);
  std::cout << "http_status=" << status << " http_version=" << version << " body=" << body << "\n";
  return status == 200 ? 0 : 1;
}

// The test resolver: localhost and loopback literals resolve to 127.0.0.1;
// anything else fails, which the client surfaces as a Transport outcome.
std::string test_resolve(const std::string& host) {
  if (host == "localhost" || host == "127.0.0.1" || host == "::1") return "127.0.0.1";
  return {};
}

int llm_probe(int argc, char** argv) {
  std::string url, model = "deepseek-flash", system = "You answer briefly.", user = "Reply with the single word: ok";
  int max_tokens = 64, requests = 1, timeout_s = 120;
  bool invalid_key = false;
  bool test_resolver = false;
  int resolver_fail_first = 0;
  int new_connections_per_s = 50;
  long dns_ttl_s = 600;
  for (int index = 3; index < argc; ++index) {
    const std::string arg(argv[index]);
    if (arg == "--url" && index + 1 < argc) url = argv[++index];
    else if (arg == "--model" && index + 1 < argc) model = argv[++index];
    else if (arg == "--system" && index + 1 < argc) system = argv[++index];
    else if (arg == "--user" && index + 1 < argc) user = argv[++index];
    else if (arg == "--max-tokens" && index + 1 < argc) max_tokens = std::atoi(argv[++index]);
    else if (arg == "--requests" && index + 1 < argc) requests = std::atoi(argv[++index]);
    else if (arg == "--timeout" && index + 1 < argc) timeout_s = std::atoi(argv[++index]);
    else if (arg == "--new-connections-per-s" && index + 1 < argc) new_connections_per_s = std::max(0, std::atoi(argv[++index]));
    else if (arg == "--dns-ttl" && index + 1 < argc) dns_ttl_s = std::max(1L, std::atol(argv[++index]));
    else if (arg == "--test-resolver") test_resolver = true;
    else if (arg == "--resolver-fail-first" && index + 1 < argc) resolver_fail_first = std::atoi(argv[++index]);
    else if (arg == "--invalid-key") invalid_key = true;
  }
  if (url.empty()) { std::cerr << "probe requires --url <base-url>\n"; return 2; }
  const char* key = std::getenv("DEEPSEEK_API_KEY");
  if (!invalid_key && (!key || !*key)) { std::cout << "key=missing\n"; return 2; }
  onion::llm::Client::Options options{url, invalid_key ? std::string("sk-invalid-key") : std::string(key),
                                      timeout_s, 10, 1, 8};
  options.new_connections_per_s = static_cast<std::size_t>(new_connections_per_s);
  options.dns_cache_ttl_s = dns_ttl_s;
  if (test_resolver) {
    options.dns_resolver = [](const std::string& host, std::uint16_t) { return test_resolve(host); };
  }
  auto resolver_failures = std::make_shared<std::atomic<int>>(resolver_fail_first);
  if (resolver_fail_first > 0) {
    options.dns_resolver = [resolver_failures](const std::string& host, std::uint16_t) mutable -> std::string {
      if (resolver_failures->fetch_sub(1) > 0) return {};
      return test_resolve(host);
    };
  }
  onion::llm::Client probe_client(std::move(options));
  onion::llm::CircuitBreaker breaker;
  std::mutex mutex; std::condition_variable done; int completed = 0;
  for (int index = 0; index < requests; ++index) {
    probe_client.submit({model, system, user, max_tokens, 0.0},
      [&, index](onion::llm::Result result) {
        std::lock_guard<std::mutex> lock(mutex);
        breaker.on_result(result.kind);
        std::cout << "probe=" << index
                  << " kind=" << static_cast<int>(result.kind)
                  << " http_status=" << result.http_status
                  << " finish_reason=" << result.finish_reason
                  << " in_hit=" << result.in_hit << " in_miss=" << result.in_miss
                  << " out=" << result.out
                  << " latency_s=" << result.latency_s << "\n";
        if (++completed == requests) done.notify_one();
      });
  }
  std::unique_lock<std::mutex> lock(mutex);
  done.wait(lock, [&] { return completed == requests; });
  std::cout << "http_version=" << probe_client.negotiated_http_version()
            << " connections_opened=" << probe_client.connections_opened()
            << " dns_lookups=" << probe_client.dns_lookups()
            << " curl_version=" << onion::llm::curl_version()
            << " curl_http2=" << (onion::llm::curl_supports_http2() ? "true" : "false")
            << " breaker_open=" << (breaker.open() ? "true" : "false")
            << " breaker_reason=" << breaker.reason() << "\n";
  return 0;
}

struct WindowSink {
  std::mutex mutex;
  std::vector<onion::llm::Result> completed;
  void add(onion::llm::Result result) {
    std::lock_guard<std::mutex> lock(mutex);
    completed.push_back(std::move(result));
  }
  std::vector<onion::llm::Result> drain() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<onion::llm::Result> out;
    out.swap(completed);
    return out;
  }
};

int adaptive_bench(int argc, char** argv) {
  std::string url, model = "deepseek-flash";
  int requests = 0, start = 4, ceiling = 50, timeout_s = 120;
  double window_s = 30.0;
  for (int index = 3; index < argc; ++index) {
    const std::string arg(argv[index]);
    if (arg == "--url" && index + 1 < argc) url = argv[++index];
    else if (arg == "--model" && index + 1 < argc) model = argv[++index];
    else if (arg == "--requests" && index + 1 < argc) requests = std::atoi(argv[++index]);
    else if (arg == "--start" && index + 1 < argc) start = std::atoi(argv[++index]);
    else if (arg == "--ceiling" && index + 1 < argc) ceiling = std::atoi(argv[++index]);
    else if (arg == "--window-s" && index + 1 < argc) window_s = std::atof(argv[++index]);
    else if (arg == "--timeout" && index + 1 < argc) timeout_s = std::atoi(argv[++index]);
  }
  if (url.empty() || requests <= 0) { std::cerr << "adaptive bench requires --url and --requests\n"; return 2; }
  if (!(window_s > 0.0)) { std::cerr << "adaptive bench requires a positive --window-s\n"; return 2; }
  const char* key = std::getenv("DEEPSEEK_API_KEY");
  onion::llm::Client client({url, key ? std::string(key) : std::string(), timeout_s, 10, 1, static_cast<std::size_t>(ceiling)});
  onion::llm::Limiter limiter(start, ceiling, window_s);
  onion::llm::CircuitBreaker breaker;
  WindowSink sink;
  std::atomic<int> submitted{0};
  std::atomic<int> finished{0};
  std::atomic<std::uint64_t> tokens_hit{0};
  std::atomic<std::uint64_t> tokens_miss{0};
  std::atomic<std::uint64_t> tokens_out{0};
  const auto begin = std::chrono::steady_clock::now();
  std::uint64_t window_start = 0;
  const auto report = [&](int window, int used, std::vector<onion::llm::Result> results) {
    std::vector<double> latencies;
    std::size_t ok = 0, rate_limited = 0, server_errors = 0, timeouts = 0, transport = 0, other = 0;
    for (const auto& result : results) {
      latencies.push_back(result.latency_s);
      switch (result.kind) {
        case onion::llm::Kind::Ok: case onion::llm::Kind::Truncated: ++ok; break;
        case onion::llm::Kind::RateLimited: ++rate_limited; break;
        case onion::llm::Kind::ServerError: ++server_errors; break;
        case onion::llm::Kind::Timeout: ++timeouts; break;
        case onion::llm::Kind::Transport: ++transport; break;
        default: ++other; break;
      }
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double fraction) {
      return latencies.empty() ? 0.0 : latencies[static_cast<std::size_t>((latencies.size() - 1) * fraction)];
    };
    std::cout << "window=" << window << " window_s=" << window_start
              << " limit_used=" << used << " limit_after=" << limiter.limit()
              << " calls=" << results.size()
              << " ok=" << ok << " rate_limited=" << rate_limited << " server_errors=" << server_errors
              << " timeouts=" << timeouts << " transport=" << transport << " other=" << other
              << " p50_s=" << percentile(.50) << " p95_s=" << percentile(.95)
              << " submitted=" << submitted.load() << " finished=" << finished.load()
              << " connections_opened=" << client.connections_opened()
              << " http_version=" << client.negotiated_http_version()
              << " breaker_open=" << (breaker.open() ? "true" : "false") << "\n";
  };

  for (int window = 0; submitted.load() < requests; ++window) {
    const int used = limiter.begin_window(window_start);
    const auto window_begin = std::chrono::steady_clock::now();
    // Fill the window continuously: the limit can be small after a backoff,
    // and a single submit pass per window would crawl at low limits.
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - window_begin).count() < window_s &&
           finished.load() < requests) {
      while (submitted.load() < requests && client.in_flight() < static_cast<std::size_t>(used)) {
        client.submit({model, "You answer in at most five words.", "Say ok", 16, 0.0},
          [&](onion::llm::Result result) {
            breaker.on_result(result.kind);
            tokens_hit += result.in_hit;
            tokens_miss += result.in_miss;
            tokens_out += result.out;
            sink.add(std::move(result));
            ++finished;
          });
        ++submitted;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto results = sink.drain();
    onion::llm::LimitWindow stats;
    stats.ended_at_s = window_start + 30;
    stats.calls = results.size();
    std::vector<double> latencies;
    for (const auto& result : results) {
      if (result.kind == onion::llm::Kind::RateLimited) ++stats.rate_limited;
      if (result.kind == onion::llm::Kind::ServerError) ++stats.server_errors;
      if (result.kind == onion::llm::Kind::Timeout) ++stats.timeouts;
      if (result.kind == onion::llm::Kind::Transport) ++stats.connect_failures;
      if (result.kind == onion::llm::Kind::Ok) {
        stats.completed_out_tokens += result.out;
        latencies.push_back(result.latency_s);
      }
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double fraction) {
      return latencies.empty() ? 0.0
          : latencies[static_cast<std::size_t>((latencies.size() - 1) * fraction)];
    };
    stats.median_latency_s = latencies.empty() ? 0.0 : percentile(.50);
    stats.p95_latency_s = latencies.empty() ? 0.0 : percentile(.95);
    limiter.on_window(stats);
    report(window, used, std::move(results));
    window_start = static_cast<std::uint64_t>((window + 1) * window_s);
  }
  std::cout << "tokens_in_hit=" << tokens_hit.load() << " tokens_in_miss=" << tokens_miss.load()
            << " tokens_out=" << tokens_out.load() << " total_requests=" << submitted.load() << "\n";
  return 0;
}
}  // namespace

#include "commands.hpp"

int llm_main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: data_depot llm bench --url <fake-server-url> --requests N\n";
    return 2;
  }
  if (std::string(argv[2]) == "limiter-trace") return limiter_trace(argc, argv);
  if (std::string(argv[2]) == "models") return llm_models(argc, argv);
  if (std::string(argv[2]) == "probe") return llm_probe(argc, argv);
  std::string url;
  int requests = 0;
  int timeout_s = 120;
  int connect_timeout_s = 10;
  int limit = 1000;
  int adaptive_start = 4;
  int adaptive_ceiling = 50;
  bool adaptive = false;
  std::string budget_limit = "10";
  for (int index = 2; index < argc; ++index) {
    const std::string arg(argv[index]);
    if (arg == "bench") continue;
    if (arg == "--url" && index + 1 < argc) url = argv[++index];
    else if (arg == "--requests" && index + 1 < argc) requests = std::atoi(argv[++index]);
    else if (arg == "--timeout" && index + 1 < argc) timeout_s = std::atoi(argv[++index]);
    else if (arg == "--connect-timeout" && index + 1 < argc) connect_timeout_s = std::atoi(argv[++index]);
    else if (arg == "--limit" && index + 1 < argc) limit = std::atoi(argv[++index]);
    else if (arg == "--budget" && index + 1 < argc) budget_limit = argv[++index];
    else if (arg == "--adaptive") adaptive = true;
    else if (arg == "--start" && index + 1 < argc) adaptive_start = std::atoi(argv[++index]);
    else if (arg == "--ceiling" && index + 1 < argc) adaptive_ceiling = std::atoi(argv[++index]);
  }
  if (adaptive) return adaptive_bench(argc, argv);
  if (url.empty() || requests <= 0) {
    std::cerr << "bench requires --url and --requests\n";
    return 2;
  }
  if (limit <= 0) {
    std::cerr << "bench requires a positive --limit\n";
    return 2;
  }
  onion::llm::Client client({url, {}, timeout_s, connect_timeout_s, 1, static_cast<std::size_t>(limit)});
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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr
        << "usage: data_depot llm|node|campaign|review|dev-import|export-csv ...\n";
    return 2;
  }
  const std::string command(argv[1]);
  if (command == "llm") return llm_main(argc, argv);
  if (command == "node") return node_cmd(argc, argv);
  if (command == "campaign") return campaign_cmd(argc, argv);
  if (command == "review") return review_cmd(argc, argv);
  if (command == "dev-import") return dev_import_cmd(argc, argv);
  if (command == "export-csv") return export_csv_cmd(argc, argv);
  if (command == "admin") return admin_cmd(argc, argv);
  if (command == "genesis-import") return genesis_import_cmd(argc, argv);
  std::cerr << "unknown command: " << command << "\n";
  return 2;
}
