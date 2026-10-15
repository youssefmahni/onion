#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>

#include "api/api.hpp"
#include "state/state.hpp"

namespace {

using onion::api::Server;
using onion::state::StateWriter;

class ApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("onion_api_test_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    StateWriter::Options options;
    options.data_dir = dir_;
    options.origin = "dev0000000000000001";
    writer_ = std::make_unique<StateWriter>(std::move(options));
    std::string error;
    ASSERT_TRUE(writer_->start(&error)) << error;

    Server::Options server_options;
    server_options.port = 0;
    server_options.token = "test-token";
    server_options.dashboard_html = "<html>dashboard</html>";
    server_options.writer = writer_.get();
    server_options.state_json = []() {
      return std::string("{\"campaigns\":[]}");
    };
    server_ = std::make_unique<Server>(std::move(server_options));
    ASSERT_TRUE(server_->start(&error)) << error;
    port_ = server_->bound_port();
    ASSERT_GT(port_, 0);
  }
  void TearDown() override {
    if (server_) server_->stop();
    if (writer_) writer_->stop();
    std::filesystem::remove_all(dir_);
  }

  std::string origin() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  std::filesystem::path dir_;
  std::unique_ptr<StateWriter> writer_;
  std::unique_ptr<Server> server_;
  int port_{0};
};

TEST_F(ApiTest, StateEndpointServesJson) {
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/api/state");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_NE(response->body.find("\"campaigns\""), std::string::npos);
}

TEST_F(ApiTest, DashboardServesAtRoot) {
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "<html>dashboard</html>");
}

TEST_F(ApiTest, PostWithoutTokenIsRefused) {
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Origin", origin()},
                              {"Content-Type", "application/json"}});
  const auto response = client.Post("/api/campaign/pause", "{\"id\":\"c1\"}",
                                    "application/json");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 401);
}

TEST_F(ApiTest, PostWithForeignOriginIsRefused) {
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers(
      {{"X-Onion-Token", "test-token"},
       {"Origin", "http://evil.example.com"},
       {"Content-Type", "application/json"}});
  const auto response = client.Post("/api/campaign/pause", "{\"id\":\"c1\"}",
                                    "application/json");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 403);
}

TEST_F(ApiTest, PostWithWrongContentTypeIsRefused) {
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers(
      {{"X-Onion-Token", "test-token"}, {"Origin", origin()}});
  const auto response = client.Post("/api/campaign/pause", "id=c1",
                                    "application/x-www-form-urlencoded");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 415);
}

TEST_F(ApiTest, PostWithCorrectHeadersIsAccepted) {
  bool paused = false;
  // Rebuild the server with a real pause hook bound to a local flag.
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "test-token";
  options.dashboard_html = "<html></html>";
  options.writer = writer_.get();
  options.state_json = []() { return std::string("{}"); };
  options.campaign_pause = [&paused](std::string id, std::string) {
    paused = (id == "c1");
    return paused;
  };
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();

  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers(
      {{"X-Onion-Token", "test-token"}, {"Origin", origin()},
       {"Content-Type", "application/json"}});
  const auto response = client.Post("/api/campaign/pause", "{\"id\":\"c1\"}",
                                    "application/json");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_TRUE(paused);
}

}  // namespace

// ---------------------------------------------------------------- D0 (plan §5)
// The dashboard-auth fixture: sessions, one-time codes and embedded assets.

namespace {

std::string cookie_from_set_cookie(const httplib::Response& response) {
  const std::string header = response.get_header_value("Set-Cookie");
  const std::size_t end = header.find(';');
  return header.substr(0, end);
}

class ApiDashTest : public ApiTest {
 protected:
  void SetUp() override {
    ApiTest::SetUp();
    // Swap the fixture server for one with dashboard auth and one static
    // asset beside index.html.
    server_->stop();
    Server::Options options;
    options.port = 0;
    options.token = "test-token";
    options.writer = writer_.get();
    options.dashboard_auth = true;
    options.auth_code_ttl = std::chrono::seconds(30);
    options.session_ttl = std::chrono::seconds(30);
    options.static_files = {
        {"index.html",
         {"<html>shell</html>", "text/html", "hash-index"}},
        {"app.js", {"console.log('x');", "text/javascript", "hash-app"}},
    };
    options.state_json = []() { return std::string("{}"); };
    server_ = std::make_unique<Server>(std::move(options));
    std::string error;
    ASSERT_TRUE(server_->start(&error)) << error;
    port_ = server_->bound_port();
  }

  // Signs in through /auth and returns the session cookie value.
  std::string sign_in() {
    const std::string code = server_->issue_auth_code();
    if (code.empty()) return {};
    httplib::Client client("127.0.0.1", port_);
    const auto response = client.Get("/auth?code=" + code);
    if (!response || response->status != 302) return {};
    return cookie_from_set_cookie(*response);
  }
};

}  // namespace

TEST_F(ApiDashTest, ApiV1ReadWithoutCredentialsIs401) {
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/api/v1/version");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 401);
}

TEST_F(ApiDashTest, ApiV1ReadWithDeviceTokenIs200) {
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"X-Onion-Token", "test-token"}});
  const auto response = client.Get("/api/v1/version");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_NE(response->body.find("\"dashboard\""), std::string::npos);
}

TEST_F(ApiDashTest, AuthCodeFlowSetsCookieFlagsAndSignsIn) {
  const std::string cookie = sign_in();
  EXPECT_NE(cookie.find("onion_session="), std::string::npos);
  // The session cookie grants /api/v1 reads.
  httplib::Client session_client("127.0.0.1", port_);
  session_client.set_default_headers({{"Cookie", cookie}});
  const auto response = session_client.Get("/api/v1/version");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
}

TEST_F(ApiDashTest, SetCookieHeaderCarriesFlags) {
  const std::string code = server_->issue_auth_code();
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/auth?code=" + code);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 302);
  const std::string set_cookie = response->get_header_value("Set-Cookie");
  EXPECT_NE(set_cookie.find("HttpOnly"), std::string::npos);
  EXPECT_NE(set_cookie.find("SameSite=Strict"), std::string::npos);
  EXPECT_NE(set_cookie.find("Path=/"), std::string::npos);
  EXPECT_EQ(response->get_header_value("Location"), "/");
}

TEST_F(ApiDashTest, BadAuthCodeIs401) {
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/auth?code=not-a-real-code");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 401);
}

TEST_F(ApiDashTest, AuthCodeIsOneShot) {
  const std::string code = server_->issue_auth_code();
  httplib::Client client("127.0.0.1", port_);
  const auto first = client.Get("/auth?code=" + code);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->status, 302);
  const auto second = client.Get("/auth?code=" + code);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->status, 401);
}

TEST_F(ApiDashTest, AuthCodeExpiresAfterTtl) {
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "test-token";
  options.writer = writer_.get();
  options.dashboard_auth = true;
  options.auth_code_ttl = std::chrono::milliseconds(50);
  options.session_ttl = std::chrono::seconds(30);
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();

  const std::string code = server_->issue_auth_code();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/auth?code=" + code);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 401);
}

TEST_F(ApiDashTest, SessionExpiresAfterInactivity) {
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "test-token";
  options.writer = writer_.get();
  options.dashboard_auth = true;
  options.auth_code_ttl = std::chrono::seconds(30);
  options.session_ttl = std::chrono::milliseconds(50);
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();

  const std::string code = server_->issue_auth_code();
  httplib::Client client("127.0.0.1", port_);
  const auto auth = client.Get("/auth?code=" + code);
  ASSERT_TRUE(auth);
  EXPECT_EQ(auth->status, 302);
  const std::string cookie = cookie_from_set_cookie(*auth);

  httplib::Client session_client("127.0.0.1", port_);
  session_client.set_default_headers({{"Cookie", cookie}});
  const auto fresh = session_client.Get("/api/v1/version");
  ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh->status, 200);

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  const auto stale = session_client.Get("/api/v1/version");
  ASSERT_TRUE(stale);
  EXPECT_EQ(stale->status, 401);
}

TEST_F(ApiTest, AuthRouteIs404WhenDashboardAuthIsOff) {
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/auth?code=anything");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 404);
}

TEST_F(ApiDashTest, StaticAssetsServeWithEtagAndNoCache) {
  httplib::Client client("127.0.0.1", port_);
  const auto index = client.Get("/");
  ASSERT_TRUE(index);
  EXPECT_EQ(index->status, 200);
  EXPECT_EQ(index->body, "<html>shell</html>");
  EXPECT_EQ(index->get_header_value("ETag"), "\"hash-index\"");
  EXPECT_EQ(index->get_header_value("Cache-Control"), "no-cache");

  const auto script = client.Get("/app.js");
  ASSERT_TRUE(script);
  EXPECT_EQ(script->status, 200);
  EXPECT_EQ(script->get_header_value("ETag"), "\"hash-app\"");
  EXPECT_NE(script->get_header_value("Content-Type").find("javascript"),
            std::string::npos);

  const auto missing = client.Get("/nope.js");
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->status, 404);
}

TEST_F(ApiTest, LegacyDashboardHtmlFallsBackWhenNoAssets) {
  // The plain fixture (no static_files) still serves dashboard_html at /.
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "<html>dashboard</html>");
}

TEST_F(ApiDashTest, CspHeaderPresentOnDashboardAndApi) {
  httplib::Client client("127.0.0.1", port_);
  const auto index = client.Get("/");
  ASSERT_TRUE(index);
  const std::string csp = index->get_header_value("Content-Security-Policy");
  EXPECT_NE(csp.find("default-src 'self'"), std::string::npos);
  EXPECT_NE(csp.find("script-src 'self'"), std::string::npos);
  EXPECT_NE(csp.find("frame-ancestors 'none'"), std::string::npos);
  EXPECT_NE(csp.find("form-action 'self'"), std::string::npos);

  const auto api = client.Get("/api/v1/version");
  ASSERT_TRUE(api);
  EXPECT_EQ(api->get_header_value("Content-Security-Policy"), csp);
}
