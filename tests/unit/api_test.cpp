#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <httplib.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
    // The per-thread read-only pool keeps its handles until thread exit,
    // so the remove can fail on Windows while they drain; the runner's
    // temp cleanup reclaims the directory (same policy as the state
    // tests).
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
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

TEST_F(ApiTest, TreeEndpointPassesPathAndDepthToTheProvider) {
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "test-token";
  options.dashboard_html = "<html>dashboard</html>";
  options.writer = writer_.get();
  std::string captured_path = "unset";
  int captured_depth = -1;
  int captured_offset = -1;
  options.tree_json = [&](std::string path, int depth, int offset) {
    captured_path = path;
    captured_depth = depth;
    captured_offset = offset;
    return std::string("{\"node\":{\"path\":\"P01\"}}");
  };
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();

  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/api/tree?path=P01&depth=3&offset=7");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(captured_path, "P01");
  EXPECT_EQ(captured_depth, 3);
  EXPECT_EQ(captured_offset, 7);
  EXPECT_NE(response->body.find("\"node\""), std::string::npos);
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

// ---------------------------------------------------------------- D1 (§4.1)
// The tree fixture: a real folded tree + viewcache behind the providers.

#include "api/tree_json.hpp"
#include "viewcache/viewcache.hpp"

namespace {

void fold_node(onion::state::StateWriter& writer, std::string path,
               int layer, std::string title, std::uint64_t hlc) {
  onion::log::Record record;
  record.type = "ADD_NODE";
  record.payload = {{"path", path}, {"layer", layer}, {"title", title}};
  record.hlc = hlc;
  writer.submit(onion::state::CommitTerminal{{record}});
  writer.sync();
}

class ApiTreeTest : public ApiDashTest {
 protected:
  void SetUp() override {
    ApiDashTest::SetUp();
    fold_node(*writer_, "P01", 0, "Planning", 1);
    fold_node(*writer_, "P01-IN01", 1, "Inventory", 2);
    fold_node(*writer_, "P01-IN01-T01", 2, "Task one", 3);
    fold_node(*writer_, "P01-IN02", 1, "Other branch", 4);

    server_->stop();
    Server::Options options;
    options.port = 0;
    options.token = "test-token";
    options.writer = writer_.get();
    options.dashboard_auth = true;
    options.state_json = []() { return std::string("{}"); };
    cache_ = std::make_unique<onion::viewcache::Cache>(
        onion::viewcache::Cache::Options{
            dir_ / "viewcache.db", dir_ / "onion.db"});
    std::string error;
    ASSERT_TRUE(cache_->start(&error)) << error;
    ASSERT_TRUE(cache_->rebuild(writer_->read_db(), &error)) << error;
    options.tree_children_json =
        [this](std::string path, std::string cursor, int limit) {
          return onion::api::tree_json::children(writer_->read_db(),
                                                 cache_.get(),
                                                 std::move(path),
                                                 std::move(cursor), limit)
              .dump();
        };
    options.tree_node_json = [this](std::string path) {
      return onion::api::tree_json::node(writer_->read_db(), cache_.get(),
                                         std::move(path))
          .dump();
    };
    options.tree_search_json =
        [this](std::string query, std::string levels, std::string status,
               std::string cursor) {
          return onion::api::tree_json::search(writer_->read_db(),
                                               cache_.get(),
                                               std::move(query),
                                               std::move(levels),
                                               std::move(status),
                                               std::move(cursor))
              .dump();
        };
    options.tree_stats_json = [this]() {
      return onion::api::tree_json::stats(writer_->read_db()).dump();
    };
    options.tree_export_csv = [this](std::string path) {
      return onion::api::tree_json::export_csv(writer_->read_db(),
                                               std::move(path));
    };
    server_ = std::make_unique<Server>(std::move(options));
    ASSERT_TRUE(server_->start(&error)) << error;
    port_ = server_->bound_port();
  }
  void TearDown() override {
    if (cache_) cache_->stop();
    ApiDashTest::TearDown();
  }
  std::unique_ptr<onion::viewcache::Cache> cache_;
};

}  // namespace

TEST_F(ApiTreeTest, TreeChildrenPagesWithCursor) {
  const std::string cookie = sign_in();
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Cookie", cookie}});

  const auto first = client.Get("/api/v1/tree/children?path=P01&limit=1");
  ASSERT_TRUE(first);
  EXPECT_EQ(first->status, 200);
  auto first_body = nlohmann::json::parse(first->body);
  ASSERT_TRUE(first_body.contains("items"));
  EXPECT_EQ(first_body["items"].size(), 1u);
  EXPECT_EQ(first_body["has_more"], true);
  const std::string cursor = first_body["next_cursor"].get<std::string>();

  const auto second =
      client.Get("/api/v1/tree/children?path=P01&limit=1&cursor=" + cursor);
  ASSERT_TRUE(second);
  auto second_body = nlohmann::json::parse(second->body);
  EXPECT_EQ(second_body["items"].size(), 1u);
  EXPECT_EQ(second_body["has_more"], false);
  EXPECT_TRUE(second_body["next_cursor"].is_null());
}

TEST_F(ApiTest, TreeChildrenWithoutCredentialsIs401) {
  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/api/v1/tree/children?path=P01");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 401);
}

TEST_F(ApiTreeTest, TreeNodeIncludesSettleAndCounts) {
  const std::string cookie = sign_in();
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Cookie", cookie}});
  const auto response = client.Get("/api/v1/tree/node?path=P01-IN01");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  const auto body = nlohmann::json::parse(response->body);
  EXPECT_EQ(body["title"], "Inventory");
  EXPECT_EQ(body["children"], 1);
  EXPECT_EQ(body["subtree"], 1);
  EXPECT_TRUE(body["settle"].is_null());
  EXPECT_EQ(body["status"], "open");
}

TEST_F(ApiTreeTest, TreeNodeNotFoundReturnsErrorJson) {
  const std::string cookie = sign_in();
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Cookie", cookie}});
  const auto response = client.Get("/api/v1/tree/node?path=NOPE");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(nlohmann::json::parse(response->body)["error"], "not found");
}

TEST_F(ApiTreeTest, TreeStatsReadsTheFoldCounters) {
  const std::string cookie = sign_in();
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Cookie", cookie}});
  const auto response = client.Get("/api/v1/tree/stats");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  const auto body = nlohmann::json::parse(response->body);
  EXPECT_EQ(body["nodes_total"], 4);
  EXPECT_EQ(body["settled_total"], 0);
  EXPECT_EQ(body["levels"].size(), 3u);
}

TEST_F(ApiTreeTest, TreeSearchPrefixAndPathJump) {
  const std::string cookie = sign_in();
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Cookie", cookie}});
  const auto by_title = client.Get("/api/v1/tree/search?q=task");
  ASSERT_TRUE(by_title);
  auto title_body = nlohmann::json::parse(by_title->body);
  ASSERT_EQ(title_body["items"].size(), 1u);
  EXPECT_EQ(title_body["items"][0]["path"], "P01-IN01-T01");

  const auto jump = client.Get("/api/v1/tree/search?q=P01-IN02");
  ASSERT_TRUE(jump);
  auto jump_body = nlohmann::json::parse(jump->body);
  ASSERT_EQ(jump_body["items"].size(), 1u);
  EXPECT_EQ(jump_body["items"][0]["path"], "P01-IN02");
}

TEST_F(ApiTreeTest, TreeExportCsvStreamsSubtree) {
  const std::string cookie = sign_in();
  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Cookie", cookie}});
  const auto response = client.Get("/api/v1/tree/export.csv?path=P01");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 200);
  EXPECT_NE(response->get_header_value("Content-Type").find("text/csv"),
            std::string::npos);
  // All four nodes of the subtree, header included.
  EXPECT_EQ(std::count(response->body.begin(), response->body.end(), '\n'),
            5);
}

TEST_F(ApiTest, ViewModeServesOpenReadsWithoutAuth) {
  // A server with no dashboard auth and no token is the `view --db` mode:
  // reads are open, mutations stay closed.
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "";
  options.dashboard_auth = false;
  options.tree_stats_json = []() {
    return std::string("{\"nodes_total\":0}");
  };
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();

  httplib::Client client("127.0.0.1", port_);
  const auto stats = client.Get("/api/v1/tree/stats");
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->status, 200);

  const auto version = client.Get("/api/v1/version");
  ASSERT_TRUE(version);
  EXPECT_EQ(nlohmann::json::parse(version->body)["read_only"], true);
}

// ---------------------------------------------------------------- D2 (§4.4)

#include "api/alerts.hpp"
#include "api/control_json.hpp"

namespace {

using onion::api::alerts::Inputs;

nlohmann::json alert_ids(const nlohmann::json& body) {
  nlohmann::json ids = nlohmann::json::array();
  for (const auto& item : body.at("items"))
    ids.push_back(item.at("id").get<std::string>());
  return ids;
}

bool has_alert(const nlohmann::json& body, std::string_view id) {
  for (const auto& item : body.at("items"))
    if (item.at("id").get<std::string>() == id) return true;
  return false;
}

}  // namespace

TEST(AlertsTest, ChurnGuardPauseRaisesAndClears) {
  Inputs paused;
  paused.campaigns.push_back({"c1", "paused", "no progress"});
  const auto raised = onion::api::alerts::list(paused);
  EXPECT_TRUE(has_alert(raised, "campaign-paused-c1"));
  // The alert names a Resume action on the v1 campaigns route.
  for (const auto& item : raised.at("items"))
    if (item.at("id") == "campaign-paused-c1")
      EXPECT_EQ(item.at("actions")[0].at("path"),
                "/api/v1/campaigns/c1/resume");

  Inputs running;
  running.campaigns.push_back({"c1", "running", ""});
  const auto cleared = onion::api::alerts::list(running);
  EXPECT_FALSE(has_alert(cleared, "campaign-paused-c1"));
}

TEST(AlertsTest, ForkBudgetMemberSyncDiskReviewBreakerEachRaiseAndClear) {
  Inputs raise;
  raise.forked_batches = 2;
  raise.device_budget = Inputs::SpendBudget{"100.0", "100.0"};
  raise.member_states = {{"d1", "SUSPECT"}, {"d2", "DEAD"}};
  raise.sync = Inputs::Sync{2, 60, 15};
  raise.disk = Inputs::Disk{5.0, 100.0};
  raise.review_open = 150;
  raise.review_threshold = 100;
  raise.breaker = Inputs::Breaker{true, "transport"};
  const auto raised = onion::api::alerts::list(raise);
  EXPECT_TRUE(has_alert(raised, "forked-batches"));
  EXPECT_TRUE(has_alert(raised, "device-budget-exhausted"));
  EXPECT_TRUE(has_alert(raised, "member-SUSPECT-d1"));
  EXPECT_TRUE(has_alert(raised, "member-DEAD-d2"));
  EXPECT_TRUE(has_alert(raised, "sync-lag"));
  EXPECT_TRUE(has_alert(raised, "disk-low"));
  EXPECT_TRUE(has_alert(raised, "review-backlog"));
  EXPECT_TRUE(has_alert(raised, "circuit-breaker"));
  EXPECT_EQ(raised.at("items").size(), 8u);

  Inputs clear;
  clear.campaigns.clear();
  clear.forked_batches = 0;
  clear.device_budget = Inputs::SpendBudget{"10.0", "100.0"};
  clear.sync = Inputs::Sync{2, 10, 15};
  clear.disk = Inputs::Disk{60.0, 100.0};
  clear.review_open = 5;
  clear.review_threshold = 100;
  clear.breaker = Inputs::Breaker{false, ""};
  const auto cleared = onion::api::alerts::list(clear);
  EXPECT_EQ(cleared.at("items").size(), 0u);
}

// Summary shape over a real writer + metrics store (D2 §4.1).
TEST_F(ApiTest, SummaryReturnsLiveShape) {
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "test-token";
  options.writer = writer_.get();
  options.dashboard_auth = true;
  options.summary_json = [this]() {
    onion::api::control_json::SummaryProbes probes;
    probes.inflight = 2;
    probes.limit = 500;
    probes.device_budget_usd = "100";
    return onion::api::control_json::summary(*writer_, nullptr, probes)
        .dump();
  };
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();

  httplib::Client client("127.0.0.1", port_);
  const auto response = client.Get("/api/v1/summary");
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 401);  // the guard still applies

  httplib::Client authed("127.0.0.1", port_);
  authed.set_default_headers({{"X-Onion-Token", "test-token"}});
  const auto ok = authed.Get("/api/v1/summary");
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->status, 200);
  const auto body = nlohmann::json::parse(ok->body);
  EXPECT_EQ(body["inflight"], 2);
  EXPECT_EQ(body["limit"], 500);
  EXPECT_EQ(body["budget_usd"], "100");
  EXPECT_TRUE(body["counts"].contains("nodes"));
  EXPECT_TRUE(body["sync"].contains("rounds_completed"));
}

TEST_F(ApiDashTest, StreamServesSummaryEvents) {
  // Rebuild with a stream-enabled server (summary only; alerts optional).
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "test-token";
  options.writer = writer_.get();
  options.dashboard_auth = true;
  options.stream_enabled = true;
  options.summary_json = []() {
    return std::string("{\"inflight\":1,\"limit\":10}");
  };
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();

  const std::string cookie = sign_in();
  // Raw socket read: the stream never ends, so a client Get() would block.
#ifndef _WIN32
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  struct sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port_));
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)),
            0);
  std::string request =
      "GET /api/v1/stream HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Cookie: " +
      cookie + "\r\nAccept: text/event-stream\r\n\r\n";
  ASSERT_EQ(::send(fd, request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));
  // The headers arrive immediately; the first summary frame follows the
  // hub's 2 s tick. Accumulate until it lands (≤ 6 s).
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::string payload;
  char buffer[4096];
  for (int attempt = 0; attempt < 6; ++attempt) {
    const ssize_t read_count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (read_count > 0) payload.append(buffer, read_count);
    if (payload.find("event: summary") != std::string::npos) break;
  }
  EXPECT_NE(payload.find("event: summary"), std::string::npos);
  EXPECT_NE(payload.find("\"inflight\":1"), std::string::npos);
  ::close(fd);
#endif  // !_WIN32
}

// ---------------------------------------------------------------- D3 (§4.3)

#include "api/campaign_json.hpp"

TEST_F(ApiTest, CampaignProvidersReturnListAndDetail) {
  // Fold one campaign record through the writer.
  onion::log::Record record;
  record.type = "CAMPAIGN";
  record.payload = {{"id", "c1"},
                    {"spec", {{"id", "c1"}, {"loop_budget", "100"}}},
                    {"state", "running"}};
  writer_->submit(onion::state::CommitTerminal{{record}});
  writer_->sync();

  const auto list = onion::api::campaign_json::list(*writer_);
  ASSERT_TRUE(list.contains("items"));
  ASSERT_EQ(list["items"].size(), 1u);
  EXPECT_EQ(list["items"][0]["id"], "c1");
  EXPECT_EQ(list["items"][0]["state"], "running");
  EXPECT_TRUE(list["items"][0].contains("budget_used_usd"));

  const auto detail = onion::api::campaign_json::detail(*writer_, "c1");
  EXPECT_EQ(detail["id"], "c1");
  EXPECT_TRUE(detail.contains("settles_by_end"));
  EXPECT_TRUE(detail.contains("settles_per_level"));

  const auto missing = onion::api::campaign_json::detail(*writer_, "nope");
  EXPECT_EQ(missing["error"], "not found");
}

TEST_F(ApiTest, CampaignPlanReturnsAProjectionWithoutWrites) {
  const auto plan = onion::api::campaign_json::plan(
      *writer_,
      nlohmann::json{{"id", "c9"}, {"scope", nlohmann::json::array()},
                     {"loop_budget", 100}});
  EXPECT_TRUE(plan.contains("projected"));
  EXPECT_TRUE(plan.contains("report"));
  EXPECT_GT(plan["projected"]["total_attempts"].get<std::int64_t>(), 0);
  // Nothing was folded by a plan.
  const auto list = onion::api::campaign_json::list(*writer_);
  EXPECT_EQ(list["items"].size(), 0u);
}

TEST_F(ApiDashTest, CampaignWriteRoutesNeedTheFullGuard) {
  server_->stop();
  Server::Options options;
  options.port = 0;
  options.token = "test-token";
  options.writer = writer_.get();
  options.dashboard_auth = true;
  options.campaign_action_json = [](std::string id, std::string action,
                                    std::string body) {
    (void)body;
    return std::string("{\"op_id\":\"op-1-" + id + "-" + action + "\"}");
  };
  server_ = std::make_unique<Server>(std::move(options));
  std::string error;
  ASSERT_TRUE(server_->start(&error)) << error;
  port_ = server_->bound_port();
  const std::string cookie = sign_in();
  const std::string origin = "http://127.0.0.1:" + std::to_string(port_);

  httplib::Client client("127.0.0.1", port_);
  client.set_default_headers({{"Cookie", cookie}, {"Origin", origin},
                              {"Content-Type", "application/json"}});
  // No X-Onion-Request → 403.
  auto refused = client.Post("/api/v1/campaigns/c1/pause", "{}",
                             "application/json");
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->status, 403);

  // Full guard + idempotency key → 202, and a repeat returns the SAME op.
  client.set_default_headers({{"Cookie", cookie},
                              {"Origin", origin},
                              {"X-Onion-Request", "1"},
                              {"Content-Type", "application/json"},
                              {"Idempotency-Key", "k-1"}});
  auto accepted = client.Post("/api/v1/campaigns/c1/pause", "{}",
                              "application/json");
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->status, 202);
  EXPECT_NE(accepted->body.find("op-1-c1-pause"), std::string::npos);

  auto repeat = client.Post("/api/v1/campaigns/c1/pause", "{}",
                            "application/json");
  ASSERT_TRUE(repeat);
  EXPECT_EQ(repeat->status, 202);
  EXPECT_EQ(repeat->body, accepted->body);
}

// ---------------------------------------------------------------- D4 (§4.3)

#include "api/review_json.hpp"

TEST_F(ApiTest, ReviewListAndDetail) {
  onion::log::Record record;
  record.type = "ADD_NODE";
  record.payload = {{"path", "P01-T01"}, {"layer", 1}, {"title", "Task"}};
  writer_->submit(onion::state::CommitTerminal{{record}});
  onion::log::Record review;
  review.type = "NEEDS_REVIEW";
  review.payload = {{"path", "P01-T01"}, {"reason", "below floor"},
                    {"attempts", 3}, {"score", "0.4"}};
  writer_->submit(onion::state::CommitTerminal{{review}});
  writer_->sync();

  const auto list = onion::api::review_json::list(writer_->read_db(), "", "",
                                                  50);
  ASSERT_EQ(list["items"].size(), 1u);
  EXPECT_EQ(list["items"][0]["path"], "P01-T01");
  EXPECT_EQ(list["items"][0]["reason"], "below floor");

  const auto filtered =
      onion::api::review_json::list(writer_->read_db(), "other", "", 50);
  EXPECT_EQ(filtered["items"].size(), 0u);

  const auto detail =
      onion::api::review_json::detail(writer_->read_db(), "P01-T01");
  EXPECT_EQ(detail["reason"], "below floor");
  EXPECT_EQ(detail["title"], "Task");
}

TEST_F(ApiTest, ReviewAcceptRetryLeafEndToEnd) {
  // A review item with two proposed children.
  for (const auto& [path, layer, title] :
       {std::tuple<std::string, int, std::string>{"P01", 0, "Root"},
        {"P01-T01", 1, "Task"}, {"P01-T01-C1", 2, "Child 1"},
        {"P01-T01-C2", 2, "Child 2"}}) {
    onion::log::Record record;
    record.type = "ADD_NODE";
    record.payload = {{"path", path}, {"layer", layer}, {"title", title}};
    writer_->submit(onion::state::CommitTerminal{{record}});
  }
  onion::log::Record review;
  review.type = "NEEDS_REVIEW";
  review.payload = {{"path", "P01-T01"}, {"reason", "below floor"},
                    {"attempts", 3}, {"score", "0.4"}};
  writer_->submit(onion::state::CommitTerminal{{review}});
  writer_->sync();

  // Accept keeping only child 1: C2 is tombstoned, the node settles kept=1.
  std::string error;
  auto records = onion::api::review_json::action_records(
      writer_->read_db(), "P01-T01", "accept",
      nlohmann::json{{"children", nlohmann::json::array({0})}}, &error);
  ASSERT_EQ(records.size(), 3u);  // 1 tombstone + review + settle
  writer_->submit(onion::state::CommitTerminal{std::move(records)});
  writer_->sync();
  EXPECT_FALSE(writer_->needs_review("P01-T01"));
  const auto settle = writer_->db().settle_by_path("P01-T01", &error);
  ASSERT_TRUE(settle.has_value());
  EXPECT_EQ(settle->kept, 1);
  const auto child2 = writer_->db().node_by_path("P01-T01-C2", &error);
  ASSERT_TRUE(child2.has_value());
  EXPECT_EQ(child2->tombstone, 1);

  // Retry: re-opens the review and takes a re-expansion lock (REEKPAND
  // records fold; the review state returns to open).
  records = onion::api::review_json::action_records(
      writer_->read_db(), "P01-T01", "retry", nlohmann::json::object(),
      &error);
  ASSERT_EQ(records.size(), 2u);
  writer_->submit(onion::state::CommitTerminal{std::move(records)});
  writer_->sync();
  // Retry decides the review and opens the re-expansion lock (the engine
  // re-attempts the node); the review itself is decided.
  EXPECT_TRUE(writer_->reexpand_open("P01-T01"));
  EXPECT_EQ(writer_->read_db().review_state("P01-T01", &error),
            std::optional<std::string>("decided"));

  // Leaf: settles without children.
  records = onion::api::review_json::action_records(
      writer_->read_db(), "P01-T01", "leaf", nlohmann::json::object(),
      &error);
  ASSERT_EQ(records.size(), 2u);
  writer_->submit(onion::state::CommitTerminal{std::move(records)});
  writer_->sync();
  EXPECT_FALSE(writer_->needs_review("P01-T01"));
  const auto leaf_settle = writer_->db().settle_by_path("P01-T01", &error);
  ASSERT_TRUE(leaf_settle.has_value());
  EXPECT_EQ(leaf_settle->kept, 0);
  EXPECT_EQ(leaf_settle->end, "cannot split");
}

// ---------------------------------------------------------------- D5 (§4.1)

#include "api/fleet_json.hpp"

TEST_F(ApiTest, FleetMembersMergeProbesWithTheTable) {
  onion::log::Record record;
  record.type = "MEMBER_ADD";
  record.payload = {{"pubkey", std::string(64, '0')},
                    {"nonce", "0123456789abcdef"}};
  writer_->submit(onion::state::CommitTerminal{{record}});
  writer_->sync();

  std::vector<onion::api::fleet_json::MemberProbe> probes;
  probes.push_back({"89eb0d6a8a691dae", "SUSPECT", 12, 0.5});
  const auto members =
      onion::api::fleet_json::members(writer_->read_db(), probes);
  ASSERT_TRUE(members.contains("items"));
  ASSERT_EQ(members["items"].size(), 1u);
  EXPECT_EQ(members["items"][0]["device_id"], "89eb0d6a8a691dae");
  EXPECT_EQ(members["items"][0]["live_state"], "SUSPECT");
  EXPECT_EQ(members["items"][0]["last_seen_s"], 12);
  EXPECT_DOUBLE_EQ(members["items"][0]["lane_weight"].get<double>(), 0.5);
}
