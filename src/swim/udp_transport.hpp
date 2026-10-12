#pragma once

#include <cstdlib>
#include <string>
#include <utility>

#include "platform/platform.hpp"
#include "swim/swim.hpp"

namespace onion::swim {

// The real transport: a platform UDP socket on port 7946 (DESIGN ›
// Transport), with the LAN multicast beacon wired to the engine's
// beacon_hook. The node loop owns the socket, calls poll() each tick to
// drain inbound datagrams into the engine, and may call send_beacon() to
// emit the signed beacon payload to the group.
class UdpTransport final : public Transport {
 public:
  UdpTransport();
  ~UdpTransport() override;

  // Binds 0.0.0.0:port; if the group is non-empty, joins it for the LAN
  // beacon (failure to join is not fatal: discovery falls back to seed
  // addresses). Returns false with *error on bind failure.
  bool open(std::uint16_t port, const std::string& beacon_group,
            std::string* error);

  void send(const std::string& address, std::string_view datagram) override;
  void set_receiver(
      std::function<void(const std::string& from_address,
                         std::string datagram)> receiver) override;

  // Drains pending datagrams into the engine (call from the node loop).
  void poll();

  // Emits the beacon payload to the group (when the group was joined).
  bool send_beacon(std::string_view payload);

  std::optional<std::uint16_t> port() const;

 private:
  platform::UdpSocket socket_;
  std::string beacon_group_;
  std::function<void(const std::string&, std::string)> receiver_;
};

}  // namespace onion::swim
