#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace onion::state {
class StateWriter;
}

namespace onion::api {
inline constexpr std::string_view component_name{"api"};

// The local API on 127.0.0.1. POST endpoints require the per-device token,
// a matching Origin and Content-Type: application/json, so a web page cannot
// trigger /api/stop or /api/review.
class Server {
 public:
  struct Options {
    int port{8080};              // 0 = pick an ephemeral port
    std::string token;           // per-device token (from the key store)
    std::string dashboard_html;  // served at /
    state::StateWriter* writer{};
    // Action hooks (set by the CLI): each runs in the handler thread.
    std::function<std::string()> state_json;
    std::function<bool(std::string, std::string)> campaign_pause;
    std::function<bool(std::string, std::string)> campaign_resume;
    std::function<bool(std::string, std::string)> campaign_stop;
    std::function<bool(std::string, std::string)> review_accept;
    std::function<bool(std::string, std::string)> review_retry;
    std::function<bool(std::string, std::string)> review_reject;
    std::function<void(bool leave)> stop_node;
    // M7: submit a MEMBER_EVICT record through the running writer.
    std::function<bool(std::string, std::string)> evict;
  };

  explicit Server(Options options);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool start(std::string* error);
  void stop();
  int bound_port() const;
  bool running() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onion::api
