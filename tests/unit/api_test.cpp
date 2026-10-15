#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

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
