#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "log/log.hpp"

namespace onion::swim {

// Beacon and gossip-update signing (decision 0013): the Ed25519 signature
// covers the canonical bytes
//   device_id '\n' "beacon" '\n' "" '\n' blake2b_hex(updates_json) '\n' ts
// — the sync-request scheme with method "beacon" and an empty path, so
// membership state changes only ever travel signed while ping/ack counters
// stay unsigned. The timestamp window is ±60 s like sync requests.

std::string sign_beacon(std::string_view device_id,
                        std::string_view updates_json,
                        std::int64_t timestamp_s, log::Signer& signer);

bool beacon_signature_valid(std::string_view device_id,
                            std::string_view updates_json,
                            std::string_view signature_hex,
                            const std::vector<std::uint8_t>& public_key,
                            std::int64_t timestamp_s, std::int64_t now_s);

}  // namespace onion::swim
