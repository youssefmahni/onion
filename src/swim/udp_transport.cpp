#include "swim/udp_transport.hpp"

namespace onion::swim {

UdpTransport::UdpTransport() = default;
UdpTransport::~UdpTransport() = default;

bool UdpTransport::open(std::uint16_t port, const std::string& beacon_group,
                        std::string* error) {
  if (!socket_.bind(port, error)) return false;
  beacon_group_ = beacon_group;
  if (!beacon_group_.empty()) {
    std::string join_error;
    // A failed join is not fatal: some routers isolate multicast, and
    // discovery falls back to the seeds carried in invitations.
    socket_.join_group(beacon_group_, &join_error);
  }
  return true;
}

void UdpTransport::send(const std::string& address,
                        std::string_view datagram) {
  // Member addresses are host:port; the transport splits them.
  const std::size_t colon = address.rfind(':');
  if (colon == std::string::npos) return;
  const std::string host = address.substr(0, colon);
  const std::string port_text = address.substr(colon + 1);
  char* end = nullptr;
  const unsigned long port_number =
      std::strtoul(port_text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || port_number == 0 ||
      port_number > 65535)
    return;
  socket_.send(host, static_cast<std::uint16_t>(port_number), datagram);
}

void UdpTransport::set_receiver(
    std::function<void(const std::string& from_address,
                       std::string datagram)> receiver) {
  receiver_ = std::move(receiver);
}

void UdpTransport::poll() {
  if (!receiver_) return;
  // Cap the per-tick drain so a flooded socket cannot starve the node
  // loop; remaining datagrams wait for the next tick.
  for (int drained = 0; drained < 64; drained++) {
    const auto datagram = socket_.recv();
    if (!datagram) return;
    if (datagram->bytes.empty()) continue;
    receiver_(datagram->from_address, std::move(datagram->bytes));
  }
}

bool UdpTransport::send_beacon(std::string_view payload) {
  if (beacon_group_.empty()) return false;
  return socket_.send(beacon_group_, 7946, payload);
}

std::optional<std::uint16_t> UdpTransport::port() const {
  return socket_.port();
}

}  // namespace onion::swim
