#include "api.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "platform/platform.hpp"

namespace onion::api {

// Every dashboard response carries the plan's CSP: no inline anything, no
// remote origins, frames forbidden. JSON responses keep it too — harmless
// and one less special case.
inline constexpr std::string_view kContentSecurityPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self'; "
    "img-src 'self' data:; connect-src 'self'; font-src 'self'; "
    "frame-ancestors 'none'; base-uri 'none'; form-action 'self'";

struct Session {
  std::chrono::steady_clock::time_point last_active;
};

struct Server::Impl {
  Options options;
  std::unique_ptr<httplib::Server> server;
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<int> port{0};

  // Dashboard sign-in state (dashboard_auth). Both maps are guarded by
  // auth_mutex; the pending code is one-shot and short-lived, sessions
  // expire after session_ttl of inactivity.
  std::mutex auth_mutex;
  std::map<std::string, std::chrono::steady_clock::time_point> pending_codes{};
  std::map<std::string, Session> sessions{};

  // SSE hub (plan §4.2): one broadcaster thread pushes summary frames every
  // 2 s plus alert/event diffs to every subscriber's queue.
  struct Subscriber {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> frames;
    bool closed{false};
  };
  std::mutex stream_mutex;
  std::vector<std::weak_ptr<Subscriber>> subscribers;
  std::int64_t stream_event_seq{0};
  std::map<std::string, std::pair<int, std::string>> idempotent;
  std::thread stream_thread;
  std::atomic<bool> stream_running{false};
};

namespace {

std::string hex_of(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const unsigned char c : bytes)
    out += digits[c >> 4], out += digits[c & 0x0f];
  return out;
}

void set_csp(httplib::Response& response) {
  response.set_header("Content-Security-Policy",
                      std::string(kContentSecurityPolicy));
}

// The cookie holding the session id, or an empty string.
std::string session_cookie(const httplib::Request& request) {
  if (!request.has_header("Cookie")) return {};
  const std::string cookies = request.get_header_value("Cookie");
  const std::string_view needle = "onion_session=";
  const std::size_t start = cookies.find(needle);
  if (start == std::string::npos) return {};
  const std::size_t value_start = start + needle.size();
  const std::size_t end = cookies.find(';', value_start);
  return cookies.substr(value_start, end == std::string::npos
                                          ? std::string::npos
                                          : end - value_start);
}

}  // namespace

// §5.2/§5.3: a valid session cookie OR the device token grants access.
// A server without dashboard auth and without a token (the read-only
// `view --db` mode) serves open reads: it has no mutating endpoints.
bool Server::authenticated(const httplib::Request& request) {
  std::string token;
  if (request.has_header("X-Onion-Token"))
    token = request.get_header_value("X-Onion-Token");
  else if (request.has_param("token"))
    token = request.get_param_value("token");
  if (impl_->options.token.empty() && !impl_->options.dashboard_auth)
    return true;  // view mode: read-only, nothing to protect
  if (!impl_->options.token.empty() && token == impl_->options.token)
    return true;
  if (!impl_->options.dashboard_auth) return false;
  const std::string id = session_cookie(request);
  if (id.empty()) return false;
  std::lock_guard lock(impl_->auth_mutex);
  const auto found = impl_->sessions.find(id);
  if (found == impl_->sessions.end()) return false;
  const auto now = std::chrono::steady_clock::now();
  if (now - found->second.last_active > impl_->options.session_ttl) {
    impl_->sessions.erase(found);
    return false;
  }
  found->second.last_active = now;
  return true;
}

bool authorized(const httplib::Request& request,
                const Server::Options& options, int bound_port,
                httplib::Response& response) {
  // Token: X-Onion-Token header or ?token= query parameter.
  std::string token;
  if (request.has_header("X-Onion-Token"))
    token = request.get_header_value("X-Onion-Token");
  else if (request.has_param("token"))
    token = request.get_param_value("token");
  if (token.empty() || token != options.token) {
    response.status = 401;
    response.set_content("{\"error\":\"missing or wrong token\"}",
                         "application/json");
    return false;
  }
  // Origin: only the API's own origin may drive POSTs.
  std::string origin;
  if (request.has_header("Origin"))
    origin = request.get_header_value("Origin");
  const std::string expected =
      "http://127.0.0.1:" + std::to_string(bound_port);
  if (origin != expected) {
    response.status = 403;
    response.set_content("{\"error\":\"foreign origin\"}", "application/json");
    return false;
  }
  // Content-Type: JSON only, so no form spoofing.
  const std::string content_type =
      request.has_header("Content-Type") ? request.get_header_value("Content-Type")
                                          : std::string{};
  if (content_type.find("application/json") == std::string::npos) {
    response.status = 415;
    response.set_content("{\"error\":\"content type must be application/json\"}",
                          "application/json");
    return false;
  }
  return true;
}

nlohmann::json body_or_error(const httplib::Request& request,
                             httplib::Response& response) {
  auto json = nlohmann::json::parse(request.body, nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    response.status = 400;
    response.set_content("{\"error\":\"invalid JSON body\"}",
                         "application/json");
  }
  return json;
}

Server::Server(Options options)
    : impl_(new Impl{std::move(options)}) {}
Server::~Server() { stop(); }

std::string Server::issue_auth_code() {
  if (!impl_->options.dashboard_auth) return {};
  std::string code;
  try {
    code = hex_of(onion::platform::secure_random(16));
  } catch (const std::exception&) {
    return {};
  }
  std::lock_guard lock(impl_->auth_mutex);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = impl_->pending_codes.begin();
       it != impl_->pending_codes.end();) {
    if (now - it->second > impl_->options.auth_code_ttl)
      it = impl_->pending_codes.erase(it);
    else
      ++it;
  }
  impl_->pending_codes.emplace(code, now);
  return code;
}

bool Server::start(std::string* error) {
  impl_->server = std::make_unique<httplib::Server>();

  impl_->server->Get("/api/state", [this](const httplib::Request&,
                                          httplib::Response& response) {
    set_csp(response);
    response.set_content(impl_->options.state_json
                             ? impl_->options.state_json()
                             : "{\"error\":\"no state provider\"}",
                         "application/json");
  });

  // The lazy tree endpoint (dashboard): GET /api/tree?path=<path>&depth=<n>.
  // Depth defaults to 2 and clamps to [0, 6] inside the provider.
  impl_->server->Get("/api/tree", [this](const httplib::Request& request,
                                         httplib::Response& response) {
    set_csp(response);
    if (!impl_->options.tree_json) {
      response.status = 404;
      response.set_content("{\"error\":\"no tree provider\"}",
                           "application/json");
      return;
    }
    std::string path;
    if (request.has_param("path")) path = request.get_param_value("path");
    int depth = 2;
    if (request.has_param("depth")) {
      try {
        depth = std::stoi(request.get_param_value("depth"));
      } catch (const std::exception&) {
        depth = 2;
      }
    }
    int offset = 0;
    if (request.has_param("offset")) {
      try {
        offset = std::max(0, std::stoi(request.get_param_value("offset")));
      } catch (const std::exception&) {
        offset = 0;
      }
    }
    try {
      response.set_content(
          impl_->options.tree_json(std::move(path), depth, offset),
          "application/json");
    } catch (const std::exception& exception) {
      response.status = 500;
      response.set_content(
          std::string("{\"error\":\"") + exception.what() + "\"}",
          "application/json");
    }
  });


  // Embedded static files (web/ at build time): / serves index.html; the
  // rest are served by name. ETag = content hash; always revalidated.
  const auto serve_static = [this](const httplib::Request& request,
                                   httplib::Response& response) {
    set_csp(response);
    std::string name = "index.html";
    if (!request.path.empty() && request.path != "/")
      name = request.path.substr(1);
    const auto& files = impl_->options.static_files;
    const auto found = files.find(name);
    if (found == files.end()) {
      if (name == "index.html" && !impl_->options.dashboard_html.empty()) {
        response.set_content(impl_->options.dashboard_html, "text/html");
        return;
      }
      response.status = 404;
      response.set_content("not found", "text/plain");
      return;
    }
    response.set_header("ETag", "\"" + found->second.hash + "\"");
    response.set_header("Cache-Control", "no-cache");
    response.set_content(found->second.bytes, found->second.mime);
  };

  // /api/v1: the new namespace. Every route goes through the same guard:
  // a valid session or the device token (§5.2). The version probe doubles
  // as the skeleton's proof endpoint and the shell's connectivity check.
  const auto guard_api_v1 = [this](const httplib::Request& request,
                                   httplib::Response& response) -> bool {
    set_csp(response);
    if (authenticated(request)) return true;
    response.status = 401;
    response.set_content("{\"error\":\"not authenticated\"}",
                         "application/json");
    return false;
  };
  impl_->server->Get("/api/v1/version", [this, guard_api_v1](
                                            const httplib::Request& request,
                                            httplib::Response& response) {
    if (!guard_api_v1(request, response)) return;
    nlohmann::json body;
    body["version"] = "0.1";
    body["phase"] = "d0";
    body["dashboard"] = true;
    body["read_only"] = !impl_->options.dashboard_auth &&
                         impl_->options.token.empty();
    response.set_content(body.dump(), "application/json");
  });

  // D1 tree reads (plan §4.1): keyset cursors, bounded pages.
  const auto tree_guard = [this, guard_api_v1](
                              const httplib::Request& request,
                              httplib::Response& response) -> bool {
    return guard_api_v1(request, response);
  };
  impl_->server->Get(
      "/api/v1/tree/children",
      [this, tree_guard](const httplib::Request& request,
                         httplib::Response& response) {
        if (!tree_guard(request, response)) return;
        if (!impl_->options.tree_children_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no tree provider\"}",
                               "application/json");
          return;
        }
        const std::string path = request.get_param_value("path");
        const std::string cursor = request.get_param_value("cursor");
        int limit = 50;
        if (request.has_param("limit")) {
          try {
            limit = std::clamp(std::stoi(request.get_param_value("limit")),
                               1, 200);
          } catch (const std::exception&) {
            limit = 50;
          }
        }
        try {
          response.set_content(
              impl_->options.tree_children_json(path, cursor, limit),
              "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      "/api/v1/tree/node",
      [this, tree_guard](const httplib::Request& request,
                         httplib::Response& response) {
        if (!tree_guard(request, response)) return;
        if (!impl_->options.tree_node_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no tree provider\"}",
                               "application/json");
          return;
        }
        const std::string path = request.get_param_value("path");
        try {
          response.set_content(impl_->options.tree_node_json(path),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      "/api/v1/tree/search",
      [this, tree_guard](const httplib::Request& request,
                         httplib::Response& response) {
        if (!tree_guard(request, response)) return;
        if (!impl_->options.tree_search_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no tree provider\"}",
                               "application/json");
          return;
        }
        const std::string q = request.get_param_value("q");
        const std::string levels = request.get_param_value("levels");
        const std::string status = request.get_param_value("status");
        const std::string cursor = request.get_param_value("cursor");
        try {
          response.set_content(
              impl_->options.tree_search_json(q, levels, status, cursor),
              "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      "/api/v1/tree/stats",
      [this, tree_guard](const httplib::Request& request,
                         httplib::Response& response) {
        if (!tree_guard(request, response)) return;
        if (!impl_->options.tree_stats_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no tree provider\"}",
                               "application/json");
          return;
        }
        try {
          response.set_content(impl_->options.tree_stats_json(),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      "/api/v1/tree/export.csv",
      [this, tree_guard](const httplib::Request& request,
                         httplib::Response& response) {
        if (!tree_guard(request, response)) return;
        if (!impl_->options.tree_export_csv) {
          response.status = 404;
          response.set_content("{\"error\":\"no tree provider\"}",
                               "application/json");
          return;
        }
        const std::string path = request.get_param_value("path");
        std::string csv;
        try {
          csv = impl_->options.tree_export_csv(path);
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
          return;
        }
        // Streamed in chunks so a large subtree never sits in one buffer.
        response.set_header("Content-Type", "text/csv; charset=utf-8");
        response.set_header(
            "Content-Disposition",
            "attachment; filename=\"tree-export.csv\"");
        std::size_t offset = 0;
        (void)offset;
        response.set_chunked_content_provider(
            "text/csv",
            [csv = std::move(csv)](std::size_t offset,
                                   httplib::DataSink& sink) {
              constexpr std::size_t kChunk = 64 * 1024;
              const std::size_t remaining = csv.size() - offset;
              if (remaining == 0) {
                sink.done();
                return true;
              }
              const std::size_t take = std::min(kChunk, remaining);
              return sink.write(csv.data() + offset, take);
            });
      });

  // Sign-in: GET /auth?code=<one-time code> swaps the code for a session
  // cookie and redirects to /.
  impl_->server->Get("/auth", [this](const httplib::Request& request,
                                     httplib::Response& response) {
    set_csp(response);
    if (!impl_->options.dashboard_auth) {
      response.status = 404;
      response.set_content("not found", "text/plain");
      return;
    }
    const std::string code = request.has_param("code")
                                 ? request.get_param_value("code")
                                 : std::string{};
    bool valid = false;
    {
      std::lock_guard lock(impl_->auth_mutex);
      const auto found = impl_->pending_codes.find(code);
      if (found != impl_->pending_codes.end()) {
        const auto now = std::chrono::steady_clock::now();
        valid = now - found->second <= impl_->options.auth_code_ttl;
        impl_->pending_codes.erase(found);  // one shot, even on expiry
      }
    }
    if (!valid) {
      response.status = 401;
      response.set_content("{\"error\":\"invalid or expired code\"}",
                           "application/json");
      return;
    }
    std::string session_id;
    try {
      session_id = hex_of(onion::platform::secure_random(16));
    } catch (const std::exception&) {
      response.status = 500;
      response.set_content("{\"error\":\"cannot create session\"}",
                           "application/json");
      return;
    }
    {
      std::lock_guard lock(impl_->auth_mutex);
      impl_->sessions.emplace(session_id,
                              Session{std::chrono::steady_clock::now()});
    }
    response.set_header("Set-Cookie",
                        "onion_session=" + session_id +
                            "; HttpOnly; SameSite=Strict; Path=/");
    response.set_redirect("/");
  });

  // D2 control reads (plan §4.1).
  const auto control_guard = [this, tree_guard](const httplib::Request& request,
                                                httplib::Response& response) {
    return tree_guard(request, response);
  };
  // §5.3: mutating /api/v1 requests need session/token + the dashboard's
  // own Origin + X-Onion-Request + JSON, plus an Idempotency-Key whose
  // repeats return the stored first response.
  const auto authorized_v1 = [this](const httplib::Request& request,
                                    httplib::Response& response) -> bool {
    set_csp(response);
    if (!authenticated(request)) {
      response.status = 401;
      response.set_content("{\"error\":\"not authenticated\"}",
                           "application/json");
      return false;
    }
    const std::string expected = "http://127.0.0.1:" +
                                 std::to_string(impl_->port.load());
    if (!request.has_header("Origin") ||
        request.get_header_value("Origin") != expected) {
      response.status = 403;
      response.set_content("{\"error\":\"foreign origin\"}",
                           "application/json");
      return false;
    }
    if (!request.has_header("X-Onion-Request") ||
        request.get_header_value("X-Onion-Request") != "1") {
      response.status = 403;
      response.set_content("{\"error\":\"missing X-Onion-Request\"}",
                           "application/json");
      return false;
    }
    const std::string content_type =
        request.has_header("Content-Type")
            ? request.get_header_value("Content-Type")
            : std::string{};
    if (content_type.find("application/json") == std::string::npos) {
      response.status = 415;
      response.set_content("{\"error\":\"content type must be application/json\"}",
                           "application/json");
      return false;
    }
    if (request.has_header("Idempotency-Key") &&
        !request.get_header_value("Idempotency-Key").empty()) {
      const std::string key = request.get_header_value("Idempotency-Key");
      std::lock_guard lock(impl_->auth_mutex);
      const auto found = impl_->idempotent.find(key);
      if (found != impl_->idempotent.end()) {
        response.status = found->second.first;
        response.set_content(found->second.second, "application/json");
        return false;  // handled: the caller must not run the action
      }
    }
    return true;
  };
  const auto record_idempotent = [this](const httplib::Request& request,
                                        int status, const std::string& body) {
    if (!request.has_header("Idempotency-Key")) return;
    const std::string key = request.get_header_value("Idempotency-Key");
    if (key.empty()) return;
    std::lock_guard lock(impl_->auth_mutex);
    impl_->idempotent[key] = {status, body};
    while (impl_->idempotent.size() > 1000) impl_->idempotent.erase(
        impl_->idempotent.begin());
  };
  const auto control_get =
      [this, control_guard](const char* path,
                            std::function<std::string()> provider) {
        impl_->server->Get(path, [this, control_guard, provider](
                                     const httplib::Request& request,
                                     httplib::Response& response) {
          if (!control_guard(request, response)) return;
          if (!provider) {
            response.status = 404;
            response.set_content("{\"error\":\"no provider\"}",
                                 "application/json");
            return;
          }
          try {
            response.set_content(provider(), "application/json");
          } catch (const std::exception& exception) {
            response.status = 500;
            response.set_content(
                std::string("{\"error\":\"") + exception.what() + "\"}",
                "application/json");
          }
        });
      };
  control_get("/api/v1/summary", [this]() {
    return impl_->options.summary_json
               ? impl_->options.summary_json()
               : std::string{};
  });
  control_get("/api/v1/alerts", [this]() {
    return impl_->options.alerts_json ? impl_->options.alerts_json()
                                      : std::string{};
  });
  impl_->server->Get("/api/v1/events",
                     [this, control_guard](const httplib::Request& request,
                                           httplib::Response& response) {
    if (!control_guard(request, response)) return;
    if (!impl_->options.events_json) {
      response.status = 404;
      response.set_content("{\"error\":\"no provider\"}",
                           "application/json");
      return;
    }
    std::int64_t since = 0;
    if (request.has_param("since")) {
      try {
        since = std::stoll(request.get_param_value("since"));
      } catch (const std::exception&) {
        since = 0;
      }
    }
    response.set_content(impl_->options.events_json(since),
                         "application/json");
  });
  impl_->server->Get("/api/v1/metrics/speed",
                     [this, control_guard](const httplib::Request& request,
                                           httplib::Response& response) {
    if (!control_guard(request, response)) return;
    if (!impl_->options.speed_json) {
      response.status = 404;
      response.set_content("{\"error\":\"no provider\"}",
                           "application/json");
      return;
    }
    const std::string range = request.get_param_value("range");
    response.set_content(
        impl_->options.speed_json(range.empty() ? "24h" : range),
        "application/json");
  });
  impl_->server->Get("/api/v1/metrics/spend",
                     [this, control_guard](const httplib::Request& request,
                                           httplib::Response& response) {
    if (!control_guard(request, response)) return;
    if (!impl_->options.spend_json) {
      response.status = 404;
      response.set_content("{\"error\":\"no provider\"}",
                           "application/json");
      return;
    }
    const std::string range = request.get_param_value("range");
    response.set_content(
        impl_->options.spend_json(range.empty() ? "24h" : range),
        "application/json");
  });

  // SSE stream (plan §4.2): summary every 2 s, alert add/clear diffs and
  // event frames as they happen. The client resumes with Last-Event-ID.
  impl_->server->Get("/api/v1/stream",
                     [this, control_guard](const httplib::Request& request,
                                           httplib::Response& response) {
    if (!control_guard(request, response)) return;
    if (!impl_->options.stream_enabled) {
      response.status = 404;
      response.set_content("{\"error\":\"stream disabled\"}",
                           "application/json");
      return;
    }
    auto sub = std::make_shared<Impl::Subscriber>();
    {
      std::lock_guard lock(impl_->stream_mutex);
      impl_->subscribers.push_back(sub);
    }
    response.set_header("Content-Type", "text/event-stream");
    response.set_header("Cache-Control", "no-cache");
    response.set_chunked_content_provider(
        "text/event-stream",
        [sub](std::size_t, httplib::DataSink& sink) {
          std::unique_lock lock(sub->mutex);
          sub->cv.wait_for(lock, std::chrono::seconds(30), [&] {
            return sub->closed || !sub->frames.empty();
          });
          if (sub->closed) {
            sink.done();
            return true;
          }
          if (sub->frames.empty()) {
            // Keep-alive comment line so proxies never time the stream
            // out while the hub is idle.
            sink.write(": keep-alive\n\n", 14);
            return true;
          }
          const std::string frame = std::move(sub->frames.front());
          sub->frames.pop_front();
          sink.write(frame.data(), frame.size());
          return true;
        });
  });

  // D3 campaigns (plan §4.1/§4.3): reads and plan-before-start actions.
  impl_->server->Get("/api/v1/campaigns",
                     [this, control_guard](const httplib::Request& request,
                                           httplib::Response& response) {
    if (!control_guard(request, response)) return;
    if (!impl_->options.campaign_list_json) {
      response.status = 404;
      response.set_content("{\"error\":\"no provider\"}",
                           "application/json");
      return;
    }
    try {
      response.set_content(impl_->options.campaign_list_json(),
                           "application/json");
    } catch (const std::exception& exception) {
      response.status = 500;
      response.set_content(
          std::string("{\"error\":\"") + exception.what() + "\"}",
          "application/json");
    }
  });
  impl_->server->Get(
      R"(/api/v1/campaigns/(.+))",
      [this, control_guard](const httplib::Request& request,
                            httplib::Response& response) {
        if (!control_guard(request, response)) return;
        if (!impl_->options.campaign_detail_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        const std::string id = request.matches[1].str();
        try {
          response.set_content(impl_->options.campaign_detail_json(id),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Post(
      "/api/v1/campaigns/plan",
      [this, authorized_v1](const httplib::Request& request,
                            httplib::Response& response) {
        if (!authorized_v1(request, response)) return;
        if (!impl_->options.campaign_plan_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          response.set_content(impl_->options.campaign_plan_json(request.body),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  const auto campaign_action_route =
      [this, authorized_v1, record_idempotent](
          const std::string& path, const std::string& action) {
        impl_->server->Post(
            path,
            [this, authorized_v1, record_idempotent, action](
                const httplib::Request& request, httplib::Response& response) {
              if (!authorized_v1(request, response)) return;
              if (!impl_->options.campaign_action_json) {
                response.status = 404;
                response.set_content("{\"error\":\"no provider\"}",
                                     "application/json");
                return;
              }
              const std::string id =
                  request.matches.empty()
                      ? std::string{}
                      : request.matches[1].str();
              try {
                const std::string body =
                    impl_->options.campaign_action_json(id, action,
                                                        request.body);
                record_idempotent(request, 202, body);
                response.status = 202;
                response.set_content(body, "application/json");
              } catch (const std::exception& exception) {
                response.status = 500;
                response.set_content(
                    std::string("{\"error\":\"") + exception.what() +
                        "\"}",
                    "application/json");
              }
            });
      };
  impl_->server->Post(
      "/api/v1/campaigns",
      [this, authorized_v1, record_idempotent](
          const httplib::Request& request, httplib::Response& response) {
        if (!authorized_v1(request, response)) return;
        if (!impl_->options.campaign_action_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          const std::string body = impl_->options.campaign_action_json(
              "", "start", request.body);
          record_idempotent(request, 202, body);
          response.status = 202;
          response.set_content(body, "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  campaign_action_route(R"(/api/v1/campaigns/(.+)/pause)", "pause");
  campaign_action_route(R"(/api/v1/campaigns/(.+)/resume)", "resume");
  campaign_action_route(R"(/api/v1/campaigns/(.+)/budget)", "budget");
  campaign_action_route(R"(/api/v1/campaigns/(.+)/settings)", "settings");
  campaign_action_route(R"(/api/v1/campaigns/(.+)/stop)", "stop");

  // D4 review queue (plan §4.1/§4.3).
  impl_->server->Get(
      "/api/v1/reviews",
      [this, control_guard](const httplib::Request& request,
                            httplib::Response& response) {
        if (!control_guard(request, response)) return;
        if (!impl_->options.review_list_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        int limit = 50;
        if (request.has_param("limit")) {
          try {
            limit = std::clamp(std::stoi(request.get_param_value("limit")),
                               1, 200);
          } catch (const std::exception&) {
            limit = 50;
          }
        }
        try {
          response.set_content(
              impl_->options.review_list_json(
                  request.get_param_value("reason"),
                  request.get_param_value("cursor"), limit),
              "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      R"(/api/v1/reviews/(.+))",
      [this, control_guard](const httplib::Request& request,
                            httplib::Response& response) {
        if (!control_guard(request, response)) return;
        if (!impl_->options.review_detail_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          response.set_content(
              impl_->options.review_detail_json(request.matches[1].str()),
              "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      "/api/v1/review-settings",
      [this, control_guard](const httplib::Request& request,
                            httplib::Response& response) {
        if (!control_guard(request, response)) return;
        if (!impl_->options.review_settings_get_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          response.set_content(impl_->options.review_settings_get_json(),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Put(
      "/api/v1/review-settings",
      [this, authorized_v1, record_idempotent](
          const httplib::Request& request, httplib::Response& response) {
        if (!authorized_v1(request, response)) return;
        if (!impl_->options.review_settings_put_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          const std::string body =
              impl_->options.review_settings_put_json(request.body);
          record_idempotent(request, 200, body);
          response.status = 200;
          response.set_content(body, "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  const auto review_action_route =
      [this, authorized_v1, record_idempotent](const std::string& action) {
        impl_->server->Post(
            R"(/api/v1/reviews/(.+)/)" + action,
            [this, authorized_v1, record_idempotent, action](
                const httplib::Request& request, httplib::Response& response) {
              if (!authorized_v1(request, response)) return;
              if (!impl_->options.review_action_json) {
                response.status = 404;
                response.set_content("{\"error\":\"no provider\"}",
                                     "application/json");
                return;
              }
              try {
                const std::string body = impl_->options.review_action_json(
                    request.matches[1].str(), action, request.body);
                record_idempotent(request, 202, body);
                response.status = 202;
                response.set_content(body, "application/json");
              } catch (const std::exception& exception) {
                response.status = 500;
                response.set_content(
                    std::string("{\"error\":\"") + exception.what() +
                        "\"}",
                    "application/json");
              }
            });
      };
  review_action_route("accept");
  review_action_route("retry");
  review_action_route("leaf");

  // D5 fleet + device (plan §4.1/§4.3).
  impl_->server->Get(
      "/api/v1/members",
      [this, control_guard](const httplib::Request& request,
                            httplib::Response& response) {
        if (!control_guard(request, response)) return;
        if (!impl_->options.members_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          response.set_content(impl_->options.members_json(),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      "/api/v1/invites",
      [this, control_guard](const httplib::Request& request,
                            httplib::Response& response) {
        if (!control_guard(request, response)) return;
        if (!impl_->options.invites_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          response.set_content(impl_->options.invites_json(),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Post(
      "/api/v1/invites",
      [this, authorized_v1, record_idempotent](
          const httplib::Request& request, httplib::Response& response) {
        if (!authorized_v1(request, response)) return;
        if (!impl_->options.invite_post_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          const std::string body =
              impl_->options.invite_post_json(request.body);
          record_idempotent(request, 200, body);
          response.status = 200;
          response.set_content(body, "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Get(
      "/api/v1/evictions",
      [this, control_guard](const httplib::Request& request,
                            httplib::Response& response) {
        if (!control_guard(request, response)) return;
        if (!impl_->options.evictions_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          response.set_content(impl_->options.evictions_json(),
                               "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Post(
      "/api/v1/evictions",
      [this, authorized_v1, record_idempotent](
          const httplib::Request& request, httplib::Response& response) {
        if (!authorized_v1(request, response)) return;
        if (!impl_->options.eviction_action_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          const std::string body = impl_->options.eviction_action_json(
              "", "propose", request.body);
          record_idempotent(request, 202, body);
          response.status = 202;
          response.set_content(body, "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  const auto eviction_action_route =
      [this, authorized_v1, record_idempotent](const std::string& action) {
        impl_->server->Post(
            R"(/api/v1/evictions/(.+)/)" + action,
            [this, authorized_v1, record_idempotent, action](
                const httplib::Request& request, httplib::Response& response) {
              if (!authorized_v1(request, response)) return;
              if (!impl_->options.eviction_action_json) {
                response.status = 404;
                response.set_content("{\"error\":\"no provider\"}",
                                     "application/json");
                return;
              }
              try {
                const std::string body = impl_->options.eviction_action_json(
                    request.matches[1].str(), action, request.body);
                record_idempotent(request, 202, body);
                response.status = 202;
                response.set_content(body, "application/json");
              } catch (const std::exception& exception) {
                response.status = 500;
                response.set_content(
                    std::string("{\"error\":\"") + exception.what() +
                        "\"}",
                    "application/json");
              }
            });
      };
  eviction_action_route("sign");
  eviction_action_route("reject");
  impl_->server->Post(
      "/api/v1/device/llm-key",
      [this, authorized_v1, record_idempotent](
          const httplib::Request& request, httplib::Response& response) {
        if (!authorized_v1(request, response)) return;
        if (!impl_->options.device_llm_key_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          const std::string body =
              impl_->options.device_llm_key_json(request.body);
          record_idempotent(request, 200, body);
          response.status = 200;
          response.set_content(body, "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });
  impl_->server->Post(
      "/api/v1/device/restart-service",
      [this, authorized_v1, record_idempotent](
          const httplib::Request& request, httplib::Response& response) {
        if (!authorized_v1(request, response)) return;
        if (!impl_->options.device_restart_json) {
          response.status = 404;
          response.set_content("{\"error\":\"no provider\"}",
                               "application/json");
          return;
        }
        try {
          const std::string body =
              impl_->options.device_restart_json(request.body);
          record_idempotent(request, 202, body);
          response.status = 202;
          response.set_content(body, "application/json");
        } catch (const std::exception& exception) {
          response.status = 500;
          response.set_content(
              std::string("{\"error\":\"") + exception.what() + "\"}",
              "application/json");
        }
      });

  impl_->server->Get("/", serve_static);
  impl_->server->Get(R"(/(.+))", serve_static);

  const auto bind_post = [this](const char* path,
                                std::function<bool(std::string, std::string)> action) {
    impl_->server->Post(
        path, [this, action](const httplib::Request& request,
                             httplib::Response& response) {
          if (!authorized(request, impl_->options, impl_->port.load(),
                           response))
            return;
          auto json = body_or_error(request, response);
          if (json.is_discarded()) return;
          const std::string id = json.value("id", std::string{});
          const std::string note = json.value("note", std::string{});
          const std::string path_value = json.value("path", std::string{});
          const std::string key = id.empty() ? path_value : id;
          if (!action || key.empty()) {
            response.status = 400;
            response.set_content("{\"error\":\"bad request\"}",
                                 "application/json");
            return;
          }
          if (action(key, note)) {
            response.set_content("{\"ok\":true}", "application/json");
          } else {
            response.status = 409;
            response.set_content("{\"error\":\"action rejected\"}",
                                 "application/json");
          }
        });
  };

  // `data_depot dashboard` support: mint a fresh one-time sign-in code over
  // the device-token channel (same guard as every other POST) and return the
  // ready-to-paste URL. The code itself leaves the machine only in this
  // response, which the local CLI prints.
  impl_->server->Post("/api/dashboard/auth-code",
                      [this](const httplib::Request& request,
                             httplib::Response& response) {
    set_csp(response);
    if (!authorized(request, impl_->options, impl_->port.load(), response))
      return;
    if (!impl_->options.dashboard_auth) {
      response.status = 409;
      response.set_content("{\"error\":\"dashboard auth disabled\"}",
                           "application/json");
      return;
    }
    const std::string code = issue_auth_code();
    if (code.empty()) {
      response.status = 500;
      response.set_content("{\"error\":\"cannot mint sign-in code\"}",
                           "application/json");
      return;
    }
    const std::string url = "http://127.0.0.1:" +
                            std::to_string(impl_->port.load()) +
                            "/auth?code=" + code;
    response.set_content(nlohmann::json{{"url", url}}.dump(),
                         "application/json");
  });
  const auto hook = [](std::function<bool(std::string, std::string)>& action) {
    return [&action](std::string id, std::string note) {
      return action && action(std::move(id), std::move(note));
    };
  };
  bind_post("/api/campaign/pause", hook(impl_->options.campaign_pause));
  bind_post("/api/campaign/resume", hook(impl_->options.campaign_resume));
  bind_post("/api/campaign/stop", hook(impl_->options.campaign_stop));
  bind_post("/api/review/accept", hook(impl_->options.review_accept));
  bind_post("/api/review/retry", hook(impl_->options.review_retry));
  bind_post("/api/review/reject", hook(impl_->options.review_reject));
  // Operator-forced checkpoint: bodyless (the same guard, but no id needed).
  impl_->server->Post("/api/checkpoint",
                      [this](const httplib::Request& request,
                             httplib::Response& response) {
    if (!authorized(request, impl_->options, impl_->port.load(), response))
      return;
    auto json = body_or_error(request, response);
    if (json.is_discarded()) return;
    const auto& action = impl_->options.force_checkpoint;
    if (!action || !action("checkpoint", "")) {
      response.status = 409;
      response.set_content("{\"error\":\"checkpoint rejected\"}",
                           "application/json");
      return;
    }
    response.set_content("{\"ok\":true}", "application/json");
  });
  bind_post("/api/stop", [this](std::string body, std::string) {
    bool leave = false;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_object() && parsed.contains("leave") &&
        parsed["leave"].is_boolean())
      leave = parsed["leave"].get<bool>();
    if (impl_->options.stop_node) impl_->options.stop_node(leave);
    return true;
  });

  // The SSE hub: broadcasts summary/alerts/events to every subscriber.
  if (impl_->options.stream_enabled && impl_->options.summary_json) {
    impl_->stream_running.store(true);
    impl_->stream_thread = std::thread([this] {
      while (impl_->stream_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string summary;
        try {
          summary = impl_->options.summary_json();
        } catch (const std::exception&) {
          continue;
        }
        std::string alerts_body;
        std::set<std::string> alert_ids;
        if (impl_->options.alerts_json) {
          try {
            alerts_body = impl_->options.alerts_json();
            const auto parsed = nlohmann::json::parse(alerts_body, nullptr,
                                                      false);
            if (parsed.is_object() && parsed.contains("items")) {
              for (const auto& item : parsed["items"])
                if (item.contains("id"))
                  alert_ids.insert(item["id"].get<std::string>());
            }
          } catch (const std::exception&) {
          }
        }
        std::string events_body;
        if (impl_->options.events_json) {
          try {
            events_body = impl_->options.events_json(impl_->stream_event_seq);
            const auto parsed = nlohmann::json::parse(events_body, nullptr,
                                                      false);
            if (parsed.is_object() && parsed.contains("items")) {
              for (const auto& item : parsed["items"])
                if (item.contains("seq"))
                  impl_->stream_event_seq =
                      std::max(impl_->stream_event_seq,
                               item["seq"].get<std::int64_t>());
            }
          } catch (const std::exception&) {
          }
        }

        const auto sse = [](std::string event, std::string data) {
          return "event: " + std::move(event) + "\ndata: " +
                 std::move(data) + "\n\n";
        };
        std::vector<std::shared_ptr<Impl::Subscriber>> live;
        {
          std::lock_guard lock(impl_->stream_mutex);
          for (auto& weak : impl_->subscribers)
            if (auto sub = weak.lock()) live.push_back(sub);
        }
        for (const auto& sub : live) {
          std::lock_guard lock(sub->mutex);
          if (sub->closed) continue;
          if (sub->frames.size() < 64) {
            sub->frames.push_back(sse("summary", std::move(summary)));
            sub->frames.push_back(sse("alerts", std::move(alerts_body)));
            if (!events_body.empty())
              sub->frames.push_back(sse("events", std::move(events_body)));
          } else {
            sub->closed = true;  // a stalled client: drop it
          }
          sub->cv.notify_all();
        }
      }
    });
  }

  impl_->running.store(true);
  impl_->thread = std::thread([this] {
    if (impl_->options.port == 0) {
      const int bound =
          impl_->server->bind_to_any_port(impl_->options.host.c_str());
      impl_->port.store(bound);
      if (bound < 0) impl_->running.store(false);
    } else {
      impl_->port.store(impl_->options.port);
      if (!impl_->server->bind_to_port(impl_->options.host.c_str(),
                                       impl_->options.port))
        impl_->running.store(false);
    }
    while (impl_->running.load()) {
      if (!impl_->server->listen_after_bind()) break;
    }
  });

  // Wait for the bound port.
  for (int i = 0; i < 200 && impl_->port.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (impl_->port.load() == 0) {
    if (error) *error = "API server failed to bind to 127.0.0.1";
    impl_->running.store(false);
    if (impl_->thread.joinable()) impl_->thread.join();
    return false;
  }
  return true;
}

void Server::stop() {
  if (!impl_->running.load() && !impl_->thread.joinable() &&
      !impl_->stream_running.load())
    return;
  impl_->stream_running.store(false);
  {
    std::lock_guard lock(impl_->stream_mutex);
    for (auto& weak : impl_->subscribers)
      if (auto sub = weak.lock()) {
        std::lock_guard sub_lock(sub->mutex);
        sub->closed = true;
        sub->cv.notify_all();
      }
  }
  if (impl_->stream_thread.joinable()) impl_->stream_thread.join();
  impl_->running.store(false);
  if (impl_->server) impl_->server->stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

void Server::push_stream(std::string event, std::string data) {
  if (!impl_->stream_running.load()) return;
  std::vector<std::shared_ptr<Impl::Subscriber>> live;
  {
    std::lock_guard lock(impl_->stream_mutex);
    for (auto& weak : impl_->subscribers)
      if (auto sub = weak.lock()) live.push_back(sub);
  }
  const std::string frame =
      "event: " + event + "\ndata: " + data + "\n\n";
  for (const auto& sub : live) {
    std::lock_guard lock(sub->mutex);
    if (sub->closed) continue;
    if (sub->frames.size() < 64) {
      sub->frames.push_back(frame);
      sub->cv.notify_all();
    } else {
      sub->closed = true;
    }
  }
}

int Server::bound_port() const { return impl_->port.load(); }
bool Server::running() const { return impl_->running.load(); }

}  // namespace onion::api
