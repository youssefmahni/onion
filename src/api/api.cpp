#include "api.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace onion::api {

struct Server::Impl {
  Options options;
  std::unique_ptr<httplib::Server> server;
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<int> port{0};
};

namespace {

bool authorized(const httplib::Request& request,
                const Server::Options& options, httplib::Response& response) {
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
      "http://127.0.0.1:" + std::to_string(options.port);
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

}  // namespace

Server::Server(Options options)
    : impl_(new Impl{std::move(options), nullptr, {}, false, 0}) {}
Server::~Server() { stop(); }

bool Server::start(std::string* error) {
  impl_->server = std::make_unique<httplib::Server>();

  impl_->server->Get("/api/state", [this](const httplib::Request&,
                                          httplib::Response& response) {
    response.set_content(impl_->options.state_json
                             ? impl_->options.state_json()
                             : "{\"error\":\"no state provider\"}",
                         "application/json");
  });

  impl_->server->Get("/", [this](const httplib::Request&,
                                 httplib::Response& response) {
    response.set_content(impl_->options.dashboard_html, "text/html");
  });

  const auto bind_post = [this](const char* path,
                                std::function<bool(std::string, std::string)> action) {
    impl_->server->Post(
        path, [this, action](const httplib::Request& request,
                             httplib::Response& response) {
          if (!authorized(request, impl_->options, response)) return;
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
  bind_post("/api/stop", [this](std::string, std::string) {
    if (impl_->options.stop_node) impl_->options.stop_node();
    return true;
  });

  impl_->running.store(true);
  impl_->thread = std::thread([this] {
    const int bound =
        impl_->server->bind_to_port("127.0.0.1", impl_->options.port);
    impl_->port.store(bound);
    if (bound == 0) impl_->running.store(false);
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
  if (!impl_->running.load() && !impl_->thread.joinable()) return;
  impl_->running.store(false);
  if (impl_->server) impl_->server->stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

int Server::bound_port() const { return impl_->port.load(); }
bool Server::running() const { return impl_->running.load(); }

}  // namespace onion::api
