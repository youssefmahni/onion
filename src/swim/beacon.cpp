#include "swim/beacon.hpp"

#include <sstream>

#include <monocypher.h>
#include <monocypher-ed25519.h>

namespace onion::swim {

namespace {

std::string signed_bytes(std::string_view device_id,
                         std::string_view updates_json,
                         std::int64_t timestamp_s) {
  std::ostringstream out;
  out << device_id << '\n'
      << "beacon" << '\n'
      << '\n'
      << onion::log::blake2b_hex(updates_json) << '\n'
      << timestamp_s;
  return out.str();
}

std::vector<std::uint8_t> unhex_bytes(std::string_view hex) {
  std::vector<std::uint8_t> raw;
  raw.reserve(hex.size() / 2);
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = digit(hex[i]);
    const int lo = digit(hex[i + 1]);
    if (hi < 0 || lo < 0) return {};
    raw.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return raw;
}

}  // namespace

std::string sign_beacon(std::string_view device_id,
                        std::string_view updates_json,
                        std::int64_t timestamp_s, log::Signer& signer) {
  return signer.sign(signed_bytes(device_id, updates_json, timestamp_s));
}

bool beacon_signature_valid(std::string_view device_id,
                            std::string_view updates_json,
                            std::string_view signature_hex,
                            const std::vector<std::uint8_t>& public_key,
                            std::int64_t timestamp_s, std::int64_t now_s) {
  if (public_key.size() != 32) return false;
  if (timestamp_s < now_s - 60 || timestamp_s > now_s + 60) return false;
  const std::vector<std::uint8_t> signature = unhex_bytes(signature_hex);
  if (signature.size() != 64) return false;
  const std::string message =
      signed_bytes(device_id, updates_json, timestamp_s);
  return crypto_ed25519_check(
             signature.data(), public_key.data(),
             reinterpret_cast<const std::uint8_t*>(message.data()),
             message.size()) == 0;
}

}  // namespace onion::swim
