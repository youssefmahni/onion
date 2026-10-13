#include "log/invite.hpp"

#include <cstdlib>

#include <nlohmann/json.hpp>

#include <monocypher.h>
#include <monocypher-ed25519.h>

namespace onion::log {

namespace {



nlohmann::json payload_json(const InviteToken& token) {
  return {
      {"pubkey", token.pubkey_hex},
      {"inviter", token.inviter},
      {"expires_at_s", token.expires_at_s},
      {"nonce", token.nonce},
  };
}

}  // namespace

InviteToken issue_invite(std::string_view pubkey_hex,
                         std::string_view inviter, std::int64_t ttl_s,
                         std::string_view nonce, std::int64_t now_s,
                         Signer& signer) {
  InviteToken token;
  token.pubkey_hex = std::string(pubkey_hex);
  token.inviter = std::string(inviter);
  // Bounds enforced here so every caller mints a token the join accepts.
  token.expires_at_s = now_s + std::min(ttl_s, std::int64_t{31536000});
  token.nonce = std::string(nonce);
  token.signature = signer.sign(canonical_json(payload_json(token)));
  return token;
}

std::string invite_to_string(const InviteToken& token) {
  nlohmann::json json = payload_json(token);
  json["sig"] = token.signature;
  return json.dump();
}

std::optional<InviteToken> parse_invite(
    std::string_view token_json, std::int64_t now_s,
    const std::vector<std::uint8_t>& inviter_public_key, std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<InviteToken> {
    if (error) *error = std::move(message);
    return std::nullopt;
  };
  if (inviter_public_key.size() != 32)
    return fail("inviter public key must be 32 bytes");
  const nlohmann::json json = nlohmann::json::parse(token_json, nullptr, false);
  if (json.is_discarded() || !json.is_object())
    return fail("malformed invite token");
  if (!json.contains("pubkey") || !json["pubkey"].is_string() ||
      !json.contains("inviter") || !json["inviter"].is_string() ||
      !json.contains("expires_at_s") ||
      !json["expires_at_s"].is_number_integer() ||
      !json.contains("nonce") || !json["nonce"].is_string() ||
      !json.contains("sig") || !json["sig"].is_string())
    return fail("invite token missing fields");

  InviteToken token;
  token.pubkey_hex = json["pubkey"].get<std::string>();
  token.inviter = json["inviter"].get<std::string>();
  token.expires_at_s = json["expires_at_s"].get<std::int64_t>();
  token.nonce = json["nonce"].get<std::string>();
  token.signature = json["sig"].get<std::string>();

  if (token.pubkey_hex.size() != 64 || unhex_bytes(token.pubkey_hex).size() != 32)
    return fail("invite token public key is not 64 hex chars");
  // Even-length hex only: two distinct nonce strings must never decode to
  // the same bytes (the replay contract records the decoded nonce).
  const std::vector<std::uint8_t> nonce_bytes = unhex_bytes(token.nonce);
  if (token.nonce.size() < 16 || token.nonce.size() > 64 ||
      nonce_bytes.size() * 2 != token.nonce.size())
    return fail("invite token nonce malformed");
  if (token.expires_at_s <= now_s) return fail("invite token expired");

  const std::vector<std::uint8_t> signature = unhex_bytes(token.signature);
  if (signature.size() != 64) return fail("invite token signature malformed");
  const std::string signed_json = canonical_json(payload_json(token));
  if (crypto_ed25519_check(
          signature.data(), inviter_public_key.data(),
          reinterpret_cast<const std::uint8_t*>(signed_json.data()),
          signed_json.size()) != 0)
    return fail("invite token signature invalid");
  return token;
}

std::optional<std::int64_t> parse_ttl(std::string_view text,
                                      std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<std::int64_t> {
    if (error) *error = std::move(message);
    return std::nullopt;
  };
  if (text.empty()) return fail("empty ttl");
  constexpr std::int64_t kMaxTtlS = 1000000000;  // ~31 years
  std::int64_t multiplier = 1;
  std::string digits(text);
  while (!digits.empty() && (digits.front() == ' ' || digits.front() == '\t'))
    digits.erase(digits.begin());
  while (!digits.empty() && (digits.back() == ' ' || digits.back() == '\t'))
    digits.pop_back();
  if (!digits.empty() && digits.front() == '+')
    return fail("ttl must be a positive number of s/m/h/d");
  const char suffix = text.back();
  if (suffix == 'h') {
    multiplier = 3600;
    digits.pop_back();
  } else if (suffix == 'm') {
    multiplier = 60;
    digits.pop_back();
  } else if (suffix == 'd') {
    multiplier = 86400;
    digits.pop_back();
  }
  if (digits.empty()) return fail("ttl has no number");
  char* end = nullptr;
  const std::int64_t value = std::strtoll(digits.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || value <= 0 ||
      value > kMaxTtlS / multiplier)
    return fail("ttl must be a positive number of s/m/h/d");
  return value * multiplier;
}

}  // namespace onion::log
