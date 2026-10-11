#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "log/log.hpp"

namespace onion::log {

// M7 joining (DESIGN › Joining): an invitation token printed by an active
// member and consumed once by the joining device. The token carries the
// invited public key, the inviter, an expiry and a single-use nonce; the
// Ed25519 signature covers the canonical JSON of the payload (the same
// scheme as MEMBER_EVICT signatures). The nonce is recorded when the join
// completes (MEMBER_ADD), so a replayed token is refused.

struct InviteToken {
  std::string pubkey_hex;   // the invited device's public key (64 hex)
  std::string inviter;      // the signing device's device_id
  std::int64_t expires_at_s{};
  std::string nonce;        // random hex, single use
  std::string signature;    // hex over canonical_json(payload)
};

// ttl_s: validity window from now (the CLI parses "24h").
InviteToken issue_invite(std::string_view pubkey_hex,
                         std::string_view inviter, std::int64_t ttl_s,
                         std::string_view nonce, std::int64_t now_s,
                         Signer& signer);

// The token as the CLI prints it: compact canonical JSON.
std::string invite_to_string(const InviteToken& token);

// Parses and verifies: signature must be valid under inviter_public_key,
// the token unexpired at now_s, and the nonce well-formed. Returns the
// token, or nullopt with *error naming the rejection.
std::optional<InviteToken> parse_invite(
    std::string_view token_json, std::int64_t now_s,
    const std::vector<std::uint8_t>& inviter_public_key, std::string* error);

// "24h", "30m", "1d", or a plain number of seconds.
std::optional<std::int64_t> parse_ttl(std::string_view text,
                                      std::string* error);

}  // namespace onion::log
