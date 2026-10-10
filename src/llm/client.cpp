#include "llm/llm.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <queue>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

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

void CircuitBreaker::on_result(Kind kind) { failures_ = kind == Kind::Transport ? failures_ + 1 : 0; }
bool CircuitBreaker::open() const { return failures_ >= 20; }
int CircuitBreaker::consecutive_transport_failures() const { return failures_; }

struct Client::Impl {
  struct Task { Request request; std::function<void(Result)> done; int attempts{}; Clock::time_point ready{}; };
  struct Active { Task task; std::string payload; std::string body; long retry_after{}; Clock::time_point started; curl_slist* headers{}; };
  explicit Impl(Options opts) : options(std::move(opts)), multi(curl_multi_init()), worker(&Impl::run, this) {}
  ~Impl() { { std::lock_guard<std::mutex> lock(mutex); stopping = true; } cv.notify_all(); worker.join(); curl_multi_cleanup(multi); }
  Options options; CURLM* multi{}; std::thread worker; std::mutex mutex; std::condition_variable cv;
  std::deque<Task> queued; std::unordered_map<CURL*, std::unique_ptr<Active>> active; bool stopping{}; std::atomic<std::size_t> flight{};
  void submit(Request request, std::function<void(Result)> done) { std::lock_guard<std::mutex> lock(mutex); queued.push_back({std::move(request), std::move(done), 0, Clock::now()}); flight++; cv.notify_all(); }
  void add(Task task) {
    CURL* easy = curl_easy_init();
    auto item_ptr = std::make_unique<Active>();
    Active& item = *item_ptr;
    active.emplace(easy, std::move(item_ptr));
    item.task = std::move(task); item.started = Clock::now();
    const auto& request = item.task.request;
    item.payload = "{\"model\":\"" + json_escape(request.model) + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(request.system) + "\"},{\"role\":\"user\",\"content\":\"" + json_escape(request.user) + "\"}],\"max_tokens\":" + std::to_string(request.max_tokens) + ",\"temperature\":" + std::to_string(request.temperature) + ",\"stream\":false,\"reasoning_effort\":\"none\"}";
    item.headers = curl_slist_append(item.headers, "Content-Type: application/json");
    if (!options.api_key.empty()) item.headers = curl_slist_append(item.headers, ("Authorization: Bearer " + options.api_key).c_str());
    curl_easy_setopt(easy, CURLOPT_URL, (options.base_url + "/chat/completions").c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, item.payload.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(item.payload.size()));
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, item.headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body); curl_easy_setopt(easy, CURLOPT_WRITEDATA, &item.body);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, read_header); curl_easy_setopt(easy, CURLOPT_HEADERDATA, &item.retry_after);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, options.timeout_s); curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, easy); curl_multi_add_handle(multi, easy);
  }
  void finish(CURL* easy, CURLcode code) {
    auto found = active.find(easy); if (found == active.end()) return; Active item = std::move(*found->second); active.erase(found);
    long status = 0; curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status); curl_multi_remove_handle(multi, easy); curl_slist_free_all(item.headers); curl_easy_cleanup(easy);
    Result result; result.http_status = static_cast<int>(status); result.retry_after_s = item.retry_after;
    result.latency_s = std::chrono::duration<double>(Clock::now() - item.started).count();
    if (code == CURLE_OPERATION_TIMEDOUT) result.kind = Kind::Timeout;
    else if (code != CURLE_OK) result.kind = Kind::Transport;
    else if (status == 429) result.kind = Kind::RateLimited;
    else if (status == 503 || status >= 500) result.kind = Kind::ServerError;
    else if (status < 200 || status >= 300) result.kind = Kind::Transport;
    else { result.finish_reason = json_string(item.body, "finish_reason"); result.content = json_string(item.body, "content");
      auto hit = json_uint(item.body, "prompt_cache_hit_tokens"); auto miss = json_uint(item.body, "prompt_cache_miss_tokens"); auto output = json_uint(item.body, "completion_tokens");
      if (!hit || !miss || !output || result.finish_reason.empty()) result.kind = Kind::Malformed;
      else { result.in_hit=*hit; result.in_miss=*miss; result.out=*output; result.kind = result.finish_reason == "length" ? Kind::Truncated : Kind::Ok; }
    }
    if ((result.kind == Kind::RateLimited || result.kind == Kind::ServerError) && item.task.attempts < 3) {
      item.task.attempts++; item.task.ready = Clock::now() + std::chrono::seconds(std::max(1L, result.retry_after_s));
      std::lock_guard<std::mutex> lock(mutex); queued.push_back(std::move(item.task)); cv.notify_all(); return;
    }
    flight--; item.task.done(std::move(result));
  }
  void run() {
    while (true) {
      { std::unique_lock<std::mutex> lock(mutex); cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return stopping || !queued.empty(); });
        auto now = Clock::now(); for (auto it = queued.begin(); it != queued.end();) { if (it->ready <= now) { Task task=std::move(*it); it=queued.erase(it); lock.unlock(); add(std::move(task)); lock.lock(); } else ++it; }
        if (stopping && queued.empty() && active.empty()) break;
      }
      int running = 0; curl_multi_perform(multi, &running); curl_multi_poll(multi, nullptr, 0, 10, nullptr);
      int messages = 0; while (CURLMsg* message = curl_multi_info_read(multi, &messages)) if (message->msg == CURLMSG_DONE) finish(message->easy_handle, message->data.result);
    }
  }
};

Client::Client(Options options) {
  static std::once_flag curl_once;
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  impl_ = std::make_unique<Impl>(std::move(options));
}
Client::~Client() = default;
void Client::submit(Request request, std::function<void(Result)> done) { impl_->submit(std::move(request), std::move(done)); }
std::size_t Client::in_flight() const { return impl_->flight.load(); }
}  // namespace onion::llm
