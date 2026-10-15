#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace onion::state {
class StateWriter;
}

namespace httplib {
struct Request;
}

namespace onion::api {
inline constexpr std::string_view component_name{"api"};

// One embedded static file (web/ at build time): the raw bytes, the MIME
// type and the content hash used as the ETag.
struct StaticFile {
  std::string bytes;
  std::string mime;
  std::string hash;
};

// The local API on 127.0.0.1. POST endpoints require the per-device token,
// a matching Origin and Content-Type: application/json, so a web page cannot
// trigger /api/stop or /api/review.
//
// With `dashboard_auth` the browser-facing surface is enabled: `/auth`
// swaps a one-time code for an in-memory session cookie, and every
// `/api/v1/*` endpoint requires a valid session or the device token.
class Server {
 public:
  struct Options {
    int port{8080};              // 0 = pick an ephemeral port
    std::string host{"127.0.0.1"};  // bind address (--host; the LAN for a fleet)
    std::string token;           // per-device token (from the key store)
    std::string dashboard_html;  // served at / when no embedded assets exist
    bool dashboard_auth{false};  // /auth + session guard on /api/v1/*
    std::chrono::milliseconds auth_code_ttl{120'000};  // one-time sign-in code
    std::chrono::milliseconds session_ttl{12 * 3'600'000};  // inactivity
    std::map<std::string, StaticFile> static_files;  // embedded web assets
    state::StateWriter* writer{};
    // Action hooks (set by the CLI): each runs in the handler thread.
    std::function<std::string()> state_json;
    // The lazy tree provider (dashboard): returns the subtree at `path`
    // down `depth` levels, capped per node (has_more on overflow) and
    // pageable per node via `offset`.
    std::function<std::string(std::string path, int depth, int offset)>
        tree_json;
    // D1 /api/v1 tree providers (all run in the handler thread and read
    // through the read-only pool; absent hooks answer 404).
    std::function<std::string(std::string path, std::string cursor,
                              int limit)>
        tree_children_json;
    std::function<std::string(std::string path)> tree_node_json;
    std::function<std::string(std::string query, std::string levels,
                              std::string status, std::string cursor)>
        tree_search_json;
    std::function<std::string()> tree_stats_json;
    std::function<std::string(std::string path)> tree_export_csv;
    std::function<bool(std::string, std::string)> campaign_pause;
    std::function<bool(std::string, std::string)> campaign_resume;
    std::function<bool(std::string, std::string)> campaign_stop;
    std::function<bool(std::string, std::string)> review_accept;
    std::function<bool(std::string, std::string)> review_retry;
    std::function<bool(std::string, std::string)> review_reject;
    // Operator-forced checkpoint: snapshots the folded state so a joiner's
    // clone matches the LIVE chain (see WORKLOG 2026-10-15 fleet repair).
    std::function<bool(std::string, std::string)> force_checkpoint;
    std::function<void(bool leave)> stop_node;
  };

  explicit Server(Options options);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool start(std::string* error);
  void stop();
  int bound_port() const;
  bool running() const;

  // Mints a one-time sign-in code (dashboard_auth). The code is valid for
  // auth_code_ttl and works exactly once. Returns an empty string if the
  // OS CSPRNG fails; the caller prints the URL only on success.
  std::string issue_auth_code();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // §5.2/§5.3: a valid session cookie OR the device token grants access.
  bool authenticated(const httplib::Request& request);
};

}  // namespace onion::api
