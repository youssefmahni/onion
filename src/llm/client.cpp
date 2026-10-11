#include "llm/llm.hpp"
#include "platform/platform.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <mutex>
#include <queue>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onion::llm {
namespace {
using Clock = std::chrono::steady_clock;

std::int64_t money_units(std::string_view input) {
  // Decimal USD in trillionths; all accounting is integer arithmetic.
  bool negative = false;
  std::size_t pos = 0;
  if (!input.empty() && input.front() == '-') { negative = true; ++pos; }
  std::int64_t whole = 0;
  while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos])))
    whole = whole * 10 + (input[pos++] - '0');
  std::int64_t fraction = 0;
  int digits = 0;
  if (pos < input.size() && input[pos++] == '.') {
    while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos])) && digits < 12) {
      fraction = fraction * 10 + (input[pos++] - '0');
      ++digits;
    }
  }
  while (digits++ < 12) fraction *= 10;
  const std::int64_t value = whole * 1000000000000LL + fraction;
  return negative ? -value : value;
}

std::string money_string(std::int64_t value) {
  const bool negative = value < 0;
  std::uint64_t magnitude = static_cast<std::uint64_t>(negative ? -value : value);
  const auto whole = magnitude / 1000000000000ULL;
  auto fraction = magnitude % 1000000000000ULL;
  std::ostringstream out;
  if (negative) out << '-';
  out << whole;
  if (fraction != 0) {
    out << '.' << std::setw(12) << std::setfill('0') << fraction;
    auto result = out.str();
    while (result.back() == '0') result.pop_back();
    return result;
  }
  return out.str();
}

std::string json_escape(std::string_view text) {
  std::string out;
  for (unsigned char ch : text) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 0x0f];
        } else out += static_cast<char>(ch);
    }
  }
  return out;
}

std::optional<std::uint64_t> json_uint(std::string_view body, std::string_view key) {
  const std::regex expression("\\\"" + std::string(key) + "\\\"\\s*:\\s*([0-9]+)");
  std::smatch match;
  const std::string copy(body);
  if (!std::regex_search(copy, match, expression)) return std::nullopt;
  return static_cast<std::uint64_t>(std::stoull(match[1].str()));
}

std::string json_string(std::string_view body, std::string_view key) {
  const std::regex expression("\\\"" + std::string(key) + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
  std::smatch match;
  const std::string copy(body);
  return std::regex_search(copy, match, expression) ? match[1].str() : std::string{};
}

// One mutex per curl lock datum: the share handle serializes DNS and TLS
// session caches across every easy handle of this client (decision 0011).
std::mutex share_locks[CURL_LOCK_DATA_LAST];
void share_lock(CURL*, curl_lock_data data, curl_lock_access, void*) { share_locks[data].lock(); }
void share_unlock(CURL*, curl_lock_data data, void*) { share_locks[data].unlock(); }

struct UrlParts { std::string host; std::uint16_t port{}; };
UrlParts split_url(std::string_view url) {
  UrlParts parts;
  const std::size_t scheme = url.find("://");
  const std::size_t host_begin = scheme == std::string_view::npos ? 0 : scheme + 3;
  const std::size_t host_end = url.find_first_of(":/", host_begin);
  parts.host = std::string(url.substr(host_begin, host_end == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : host_end - host_begin));
  const bool https = scheme != std::string_view::npos && url.substr(0, scheme) == "https";
  if (host_end != std::string_view::npos && url[host_end] == ':')
    parts.port = static_cast<std::uint16_t>(std::atoi(std::string(url.substr(host_end + 1)).c_str()));
  else
    parts.port = https ? 443 : 80;
  return parts;
}

size_t write_body(char* data, size_t size, size_t count, void* opaque) {
  static_cast<std::string*>(opaque)->append(data, size * count);
  return size * count;
}

size_t read_header(char* data, size_t size, size_t count, void* opaque) {
  const std::string_view line(data, size * count);
  auto* retry_after = static_cast<long*>(opaque);
  constexpr std::string_view prefix = "Retry-After:";
  if (line.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), line.begin(),
      [](char left, char right) { return std::tolower(static_cast<unsigned char>(left)) ==
          std::tolower(static_cast<unsigned char>(right)); })) {
    *retry_after = std::max(0L, std::strtol(std::string(line.substr(prefix.size())).c_str(), nullptr, 10));
  }
  return size * count;
}

class CpuPool {
 public:
  explicit CpuPool(std::size_t workers) {
    workers = std::max<std::size_t>(1, workers);
    workers_.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index)
      workers_.emplace_back([this] { run(); });
  }
  ~CpuPool() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    cv_.notify_all();
    for (auto& worker : workers_) worker.join();
  }
  void submit(std::function<void()> job) {
    { std::lock_guard<std::mutex> lock(mutex_); jobs_.push_back(std::move(job)); }
    cv_.notify_one();
  }
 private:
  void run() {
    while (true) {
      std::function<void()> job;
      { std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_ && jobs_.empty()) return;
        job = std::move(jobs_.front()); jobs_.pop_front();
      }
      job();
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_{};
  std::vector<std::thread> workers_;
};

Result parse_result(CURLcode code, long status, long retry_after,
                    std::string body, double latency_s) {
  Result result;
  result.http_status = static_cast<int>(status);
  result.retry_after_s = retry_after;
  result.latency_s = latency_s;
  if (code == CURLE_OPERATION_TIMEDOUT) result.kind = Kind::Timeout;
  else if (code != CURLE_OK) result.kind = Kind::Transport;
  else if (status == 429) result.kind = Kind::RateLimited;
  else if (status == 401) result.kind = Kind::AuthError;
  else if (status == 402) result.kind = Kind::BalanceError;
  else if (status == 400 || status == 422) result.kind = Kind::ClientError;
  else if (status == 503 || status >= 500) result.kind = Kind::ServerError;
  else if (status < 200 || status >= 300) result.kind = Kind::Transport;
  else {
    // Parse the body properly: the assistant content is itself JSON text in
    // every funnel reply, so a regex that stops at the first escaped quote
    // silently truncates it (found in M3).
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.contains("choices") ||
        !json["choices"].is_array() || json["choices"].empty() ||
        !json["choices"][0].contains("message") ||
        !json["choices"][0]["message"].contains("content") ||
        !json.contains("usage")) {
      result.kind = Kind::Malformed;
      return result;
    }
    result.finish_reason =
        json["choices"][0].value("finish_reason", std::string{});
    result.content =
        json["choices"][0]["message"]["content"].get<std::string>();
    const auto& usage = json["usage"];
    if (!usage.contains("prompt_cache_hit_tokens") ||
        !usage.contains("prompt_cache_miss_tokens") ||
        !usage.contains("completion_tokens") || result.finish_reason.empty()) {
      result.kind = Kind::Malformed;
      return result;
    }
    result.in_hit = usage["prompt_cache_hit_tokens"].get<std::uint64_t>();
    result.in_miss = usage["prompt_cache_miss_tokens"].get<std::uint64_t>();
    result.out = usage["completion_tokens"].get<std::uint64_t>();
    result.kind = result.finish_reason == "length" ? Kind::Truncated : Kind::Ok;
  }
  return result;
}
}  // namespace

std::string meter_cost(std::uint64_t cache_hit, std::uint64_t cache_miss,
                       std::uint64_t output, const PriceSchedule& schedule,
                       bool stayed_in_one_window, bool offpeak_window) {
  const Prices& prices = stayed_in_one_window && offpeak_window ? schedule.offpeak : schedule.peak;
  const auto per_million = [&](std::uint64_t tokens, const std::string& price) {
    const auto unit_price = money_units(price);
    return static_cast<std::int64_t>((tokens / 1000000) * unit_price +
        ((tokens % 1000000) * unit_price) / 1000000);
  };
  return money_string(per_million(cache_hit, prices.cached_input) +
                      per_million(cache_miss, prices.input) + per_million(output, prices.output));
}

struct Budget::Impl {
  explicit Impl(std::string amount) : limit(money_units(amount)) {}
  mutable std::mutex mutex;
  std::int64_t limit{};
  std::int64_t spent{};
  std::int64_t reserved{};
  std::uint64_t next_id{1};
  std::unordered_map<std::uint64_t, std::int64_t> open;
};

Budget::Budget(std::string limit_usd) : impl_(std::make_shared<Impl>(std::move(limit_usd))) {}
std::optional<Reservation> Budget::reserve(std::string usd) {
  const auto amount = money_units(usd);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (amount < 0 || impl_->spent + impl_->reserved + amount > impl_->limit) return std::nullopt;
  const auto id = impl_->next_id++;
  impl_->reserved += amount;
  impl_->open.emplace(id, amount);
  return Reservation{id, money_string(amount)};
}
void Budget::settle(const Reservation& reservation, std::string actual_usd) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->open.find(reservation.id);
  if (found == impl_->open.end()) return;
  const auto actual = std::min(found->second, std::max<std::int64_t>(0, money_units(actual_usd)));
  impl_->reserved -= found->second;
  impl_->spent += actual;
  impl_->open.erase(found);
}
std::string Budget::spent_usd() const { std::lock_guard<std::mutex> lock(impl_->mutex); return money_string(impl_->spent); }
std::string Budget::reserved_usd() const { std::lock_guard<std::mutex> lock(impl_->mutex); return money_string(impl_->reserved); }
std::string Budget::remaining_usd() const { std::lock_guard<std::mutex> lock(impl_->mutex); return money_string(impl_->limit - impl_->spent - impl_->reserved); }

void CircuitBreaker::on_result(Kind kind) {
  if (kind == Kind::Ok || kind == Kind::Truncated) { failures_ = 0; credentials_ = 0; reason_.clear(); return; }
  if (kind == Kind::RateLimited) return;
  if (kind == Kind::AuthError || kind == Kind::BalanceError) {
    ++credentials_; reason_ = kind == Kind::AuthError ? "authentication" : "balance"; return;
  }
  ++failures_; reason_ = "provider-or-transport";
}
bool CircuitBreaker::open() const { return failures_ >= 20 || credentials_ >= 3; }
int CircuitBreaker::consecutive_transport_failures() const { return failures_; }
std::string CircuitBreaker::reason() const { return open() ? reason_ : std::string{}; }

struct Client::Impl {
  struct Task { Request request; std::function<void(Result)> done; int attempts{}; Clock::time_point ready{}; };
  struct Active {
    Task task;
    // These members are individually heap-owned by Active.  An Active stays
    // alive until libcurl has removed the easy handle from the multi handle.
    std::string url;
    std::string payload;
    std::string body;
    long retry_after{};
    Clock::time_point started;
    curl_slist* headers{};
    curl_slist* resolve_list{};
  };
  explicit Impl(Options opts)
      : options(std::move(opts)), multi(curl_multi_init()),
        parser_pool(std::thread::hardware_concurrency()),
        bucket(options.new_connections_per_s),
        url_parts(split_url(options.base_url)) {
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    if (options.max_concurrent_streams > 0)
      curl_multi_setopt(multi, CURLMOPT_MAX_CONCURRENT_STREAMS, options.max_concurrent_streams);
    if (options.max_host_connections > 0)
      curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, options.max_host_connections);
    if (options.max_total_connections > 0)
      curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, options.max_total_connections);
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    worker = std::thread(&Impl::run, this);
  }
  ~Impl() { { std::lock_guard<std::mutex> lock(mutex); stopping = true; } cv.notify_all(); worker.join(); curl_share_cleanup(share); curl_multi_cleanup(multi); }
  // The worker must be declared after every member it reads: C++ initializes
  // members in declaration order, not initializer-list order.
  Options options;
  CURLM* multi{};
  CURLSH* share{};
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Task> queued;
  std::unordered_map<CURL*, std::unique_ptr<Active>> active;
  bool stopping{};
  std::atomic<std::size_t> flight{};
  std::atomic<std::size_t> peak{};
  std::atomic<std::size_t> active_peak{};
  std::atomic<std::size_t> connections{};
  std::atomic<std::size_t> dns_lookups{};
  std::atomic<long> http_version{};
  CpuPool parser_pool;
  // The connection ramp and the DNS memo are touched only by the worker
  // thread, so they need no lock.
  struct ConnectionBucket {
    explicit ConnectionBucket(std::size_t rate) : rate(rate) {}
    std::size_t rate;
    double tokens = rate ? static_cast<double>(rate) : 0.0;
    Clock::time_point last{Clock::now()};
    bool try_consume() {
      if (rate == 0) return true;
      const auto now = Clock::now();
      const double elapsed = std::chrono::duration<double>(now - last).count();
      tokens = std::min(static_cast<double>(rate), tokens + elapsed * rate);
      last = now;
      if (tokens < 1.0) return false;
      tokens -= 1.0;
      return true;
    }
    void settle(long opened) {
      if (rate == 0) return;
      // A reuse (0 connections) refunds the token; extra connections pay.
      tokens = std::max(0.0, std::min(static_cast<double>(rate), tokens + 1.0 - static_cast<double>(std::max(0L, opened))));
    }
  } bucket;
  UrlParts url_parts;
  std::unordered_map<std::string, std::pair<Clock::time_point, std::string>> dns_memo;
  std::thread worker;
  std::string resolve(const std::string& host, std::uint16_t port) {
    if (!options.dns_resolver) return {};
    const auto now = Clock::now();
    auto found = dns_memo.find(host);
    if (found != dns_memo.end() && now - found->second.first < std::chrono::seconds(options.dns_cache_ttl_s))
      return found->second.second;
    dns_lookups.fetch_add(1);
    const std::string address = options.dns_resolver(host, port);
    if (address.empty()) return {};
    dns_memo[host] = {now, address};
    return address;
  }
  bool resolve_and_add(Task task) {
    const std::string address = resolve(url_parts.host, url_parts.port);
    if (options.dns_resolver && address.empty()) {
      // Resolution failed: fail the request as a Transport error without
      // handing it to libcurl (decision 0011, dns_fail_burst).
      Result result;
      result.kind = Kind::Transport;
      result.latency_s = 0.0;
      flight--;
      task.done(std::move(result));
      return false;
    }
    add(std::move(task), address);
    return true;
  }
  void submit(Request request, std::function<void(Result)> done) {
    std::lock_guard<std::mutex> lock(mutex);
    queued.push_back({std::move(request), std::move(done), 0, Clock::now()});
    const auto now = flight.fetch_add(1) + 1;
    auto observed = peak.load();
    while (observed < now && !peak.compare_exchange_weak(observed, now)) {}
    cv.notify_all();
  }
  void add(Task task, const std::string& resolved) {
    CURL* easy = curl_easy_init();
    auto item_ptr = std::make_unique<Active>();
    Active& item = *item_ptr;
    active.emplace(easy, std::move(item_ptr));
    item.task = std::move(task); item.started = Clock::now();
    const auto& request = item.task.request;
    item.url = options.base_url + "/chat/completions";
    item.payload = "{\"model\":\"" + json_escape(request.model) + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(request.system) + "\"},{\"role\":\"user\",\"content\":\"" + json_escape(request.user) + "\"}],\"max_tokens\":" + std::to_string(request.max_tokens) + ",\"temperature\":" + std::to_string(request.temperature) + ",\"stream\":false,\"reasoning_effort\":\"none\"}";
    item.headers = curl_slist_append(item.headers, "Content-Type: application/json");
    if (!options.api_key.empty()) item.headers = curl_slist_append(item.headers, ("Authorization: Bearer " + options.api_key).c_str());
    curl_easy_setopt(easy, CURLOPT_URL, item.url.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, item.payload.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(item.payload.size()));
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, item.headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body); curl_easy_setopt(easy, CURLOPT_WRITEDATA, &item.body);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, read_header); curl_easy_setopt(easy, CURLOPT_HEADERDATA, &item.retry_after);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, options.timeout_s);
    if (options.connect_timeout_s > 0)
      curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, options.connect_timeout_s);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(easy, CURLOPT_SHARE, share);
    curl_easy_setopt(easy, CURLOPT_DNS_CACHE_TIMEOUT, options.dns_cache_ttl_s);
    if (!resolved.empty()) {
      // A pinned resolution (the test hook): "host:port:address".  The list
      // must outlive the transfer: curl reads it at connect time.
      item.resolve_list = curl_slist_append(nullptr,
          (url_parts.host + ":" + std::to_string(url_parts.port) + ":" + resolved).c_str());
      curl_easy_setopt(easy, CURLOPT_RESOLVE, item.resolve_list);
    }
    curl_easy_setopt(easy, CURLOPT_PRIVATE, easy); curl_multi_add_handle(multi, easy);
    const auto now = active.size();
    auto observed = active_peak.load();
    while (observed < now && !active_peak.compare_exchange_weak(observed, now)) {}
  }
  void finish(CURL* easy, CURLcode code) {
    auto found = active.find(easy); if (found == active.end()) return; Active item = std::move(*found->second); active.erase(found);
    long status = 0; long opened = 0; long version = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(easy, CURLINFO_NUM_CONNECTS, &opened);
    curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &version);
    connections.fetch_add(static_cast<std::size_t>(std::max(0L, opened)));
    bucket.settle(opened);
    http_version.store(version);
    curl_multi_remove_handle(multi, easy); curl_slist_free_all(item.headers); curl_slist_free_all(item.resolve_list); curl_easy_cleanup(easy);
    auto parsed = std::make_shared<std::promise<Result>>();
    auto future = parsed->get_future();
    parser_pool.submit([promise = std::move(parsed), code, status, retry_after = item.retry_after,
                        body = std::move(item.body),
                        latency = std::chrono::duration<double>(Clock::now() - item.started).count()]() mutable {
      promise->set_value(parse_result(code, status, retry_after, std::move(body), latency));
    });
    Result result = future.get();
    if ((result.kind == Kind::RateLimited || result.kind == Kind::ServerError) && item.task.attempts < 3) {
      item.task.attempts++; item.task.ready = Clock::now() + std::chrono::seconds(std::max(1L, result.retry_after_s));
      std::lock_guard<std::mutex> lock(mutex); queued.push_back(std::move(item.task)); cv.notify_all(); return;
    }
    flight--; item.task.done(std::move(result));
  }
  void run() {
    while (true) {
      std::vector<Task> ready;
      { std::unique_lock<std::mutex> lock(mutex); cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return stopping || !queued.empty(); });
        const auto now = Clock::now();
        for (auto it = queued.begin(); it != queued.end() && active.size() + ready.size() < options.max_in_flight;) {
          if (it->ready <= now) { ready.push_back(std::move(*it)); it = queued.erase(it); }
          else ++it;
        }
        if (stopping && queued.empty() && active.empty()) break;
      }
      for (auto it = ready.begin(); it != ready.end(); ++it) {
        if (!bucket.try_consume()) {
          // The connection ramp is empty: put the rest back and wait for a
          // refill (the bucket refills at new_connections_per_s).
          std::lock_guard<std::mutex> lock(mutex);
          for (auto back = ready.end(); back-- != it;) queued.push_front(std::move(*back));
          break;
        }
        resolve_and_add(std::move(*it));
      }
      int running = 0; curl_multi_perform(multi, &running); curl_multi_poll(multi, nullptr, 0, 10, nullptr);
      int messages = 0; while (CURLMsg* message = curl_multi_info_read(multi, &messages)) if (message->msg == CURLMSG_DONE) finish(message->easy_handle, message->data.result);
    }
  }
};

Client::Client(Options options) {
  static std::once_flag curl_once;
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  options.max_in_flight = std::max<std::size_t>(1, std::min(options.max_in_flight,
      platform::outbound_request_ceiling()));
  impl_ = std::make_unique<Impl>(std::move(options));
}
Client::~Client() = default;
void Client::submit(Request request, std::function<void(Result)> done) { impl_->submit(std::move(request), std::move(done)); }
std::size_t Client::in_flight() const { return impl_->flight.load(); }
std::size_t Client::peak_in_flight() const { return impl_->peak.load(); }
std::size_t Client::peak_active_requests() const { return impl_->active_peak.load(); }
std::size_t Client::connections_opened() const { return impl_->connections.load(); }
std::size_t Client::dns_lookups() const { return impl_->dns_lookups.load(); }
long Client::negotiated_http_version() const { return impl_->http_version.load(); }
std::string http_get(const std::string& url, const std::string& api_key,
                    long& http_status, long& http_version) {
  CURL* easy = curl_easy_init();
  std::string body;
  curl_slist* headers = nullptr;
  if (!api_key.empty())
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
  curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_perform(easy);
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_status);
  curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &http_version);
  curl_slist_free_all(headers);
  curl_easy_cleanup(easy);
  return body;
}

bool curl_supports_http2() {
  const auto* info = curl_version_info(CURLVERSION_NOW);
  return info != nullptr && (info->features & CURL_VERSION_HTTP2) != 0;
}
std::string curl_version() {
  const auto* info = curl_version_info(CURLVERSION_NOW);
  return info != nullptr && info->version != nullptr ? info->version : "unknown";
}
}  // namespace onion::llm
