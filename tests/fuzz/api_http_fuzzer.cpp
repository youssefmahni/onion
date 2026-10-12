// libFuzzer target: HTTP request parsing as the local API receives it (C3).
// Each iteration boots a real httplib server on an OS-assigned port with the
// API's representative endpoints, sends the raw fuzzed bytes, and shuts
// down.  This exercises httplib's request-line/header parsing end to end,
// which is what the API's token/origin/content-type checks run on.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <thread>

#include <httplib.h>

namespace {
int connect_with_retry(int port) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
      return fd;
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return -1;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0 || size > 16384) return 0;
  httplib::Server server;
  server.Get("/api/state",
             [](const httplib::Request&, httplib::Response& response) {
               response.set_content("{\"node\":{}}", "application/json");
             });
  server.Post("/api/campaign/stop",
              [](const httplib::Request& request, httplib::Response& response) {
                const std::string token = request.has_header("X-Onion-Token")
                    ? request.get_header_value("X-Onion-Token")
                    : request.get_param_value("token");
                response.set_content(token == "test-token" ? "{}"
                                                           : "{\"error\":\"token\"}",
                                     "application/json");
              });
  const int port = server.bind_to_any_port("127.0.0.1");
  if (port < 0) return 0;
  std::thread thread([&server] { server.listen_after_bind(); });
  const int fd = connect_with_retry(port);
  if (fd >= 0) {
    send(fd, data, size, MSG_NOSIGNAL);
    shutdown(fd, SHUT_WR);
    char buffer[1024];
    while (recv(fd, buffer, sizeof(buffer), 0) > 0) {
    }
    close(fd);
  }
  server.stop();
  thread.join();
  return 0;
}
