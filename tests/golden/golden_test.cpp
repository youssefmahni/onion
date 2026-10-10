#include <gtest/gtest.h>

extern "C" {
#include <monocypher.h>
#include <monocypher-ed25519.h>
}

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {
using Object = std::map<std::string, struct Json>;
using Array = std::vector<struct Json>;
struct Json {
  std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object> value;
  Json(std::nullptr_t value = nullptr) : value(value) {}
  Json(bool value) : value(value) {}
  Json(std::int64_t value) : value(value) {}
  Json(const char* value) : value(std::string(value)) {}
  Json(std::string value) : value(std::move(value)) {}
  Json(Array value) : value(std::move(value)) {}
  Json(Object value) : value(std::move(value)) {}
};

std::string escape(const std::string& value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result{"\""};
  for (const unsigned char character : value) {
    switch (character) {
      case '\"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20) {
          result += "\\u00";
          result += hex[character >> 4U];
          result += hex[character & 0x0fU];
        } else {
          result += static_cast<char>(character);
        }
    }
  }
  return result + "\"";
}

std::string canonical(const Json& json) {
  if (const auto* value = std::get_if<std::nullptr_t>(&json.value)) { (void)value; return "null"; }
  if (const auto* value = std::get_if<bool>(&json.value)) return *value ? "true" : "false";
  if (const auto* value = std::get_if<std::int64_t>(&json.value)) return std::to_string(*value);
  if (const auto* value = std::get_if<std::string>(&json.value)) return escape(*value);
  if (const auto* value = std::get_if<Array>(&json.value)) {
    std::string result{"["};
    for (std::size_t index = 0; index < value->size(); ++index) {
      if (index != 0) result += ',';
      result += canonical((*value)[index]);
    }
    return result + ']';
  }
  const auto& value = std::get<Object>(json.value);
  std::string result{"{"};
  bool first = true;
  for (const auto& [key, item] : value) {
    if (!first) result += ',';
    first = false;
    result += escape(key) + ':' + canonical(item);
  }
  return result + '}';
}

std::vector<std::uint8_t> unhex(const std::string& hex) {
  std::vector<std::uint8_t> bytes;
  if (hex.size() % 2 != 0) return bytes;
  const auto digit = [](char character) -> std::optional<unsigned> {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return std::nullopt;
  };
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    const auto high = digit(hex[index]); const auto low = digit(hex[index + 1]);
    if (!high || !low) return {};
    bytes.push_back(static_cast<std::uint8_t>((*high << 4U) | *low));
  }
  return bytes;
}

std::string hex(const std::uint8_t* bytes, std::size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result; result.reserve(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    result += digits[bytes[index] >> 4U]; result += digits[bytes[index] & 0x0fU];
  }
  return result;
}

std::string fixture(const char* name) {
  std::ifstream input(std::filesystem::path(ONION_GOLDEN_DIR) / name, std::ios::binary);
  std::string value((std::istreambuf_iterator<char>(input)), {});
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
  return value;
}

std::string blake2b_256(const std::string& input) {
  std::uint8_t digest[32]{};
  crypto_blake2b(digest, sizeof(digest),
                 reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  return hex(digest, sizeof(digest));
}

std::string sign(const std::vector<std::uint8_t>& seed, const std::string& message) {
  std::uint8_t secret[64]{}; std::uint8_t public_key[32]{}; std::uint8_t signature[64]{};
  crypto_ed25519_key_pair(secret, public_key, const_cast<std::uint8_t*>(seed.data()));
  crypto_ed25519_sign(signature, secret,
                      reinterpret_cast<const std::uint8_t*>(message.data()), message.size());
  return hex(signature, sizeof(signature));
}

TEST(GoldenEd25519, Rfc8032Section71EmptyMessage) {
  const auto seed = unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  EXPECT_EQ(sign(seed, ""), fixture("rfc8032-empty-signature.hex"));
}

TEST(GoldenEd25519, Rfc8032Section71OneByteMessage) {
  const auto seed = unhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");
  EXPECT_EQ(sign(seed, std::string("\x72", 1)), fixture("rfc8032-one-byte-signature.hex"));
}

TEST(GoldenBlake2b, KnownAnswerVectors) {
  EXPECT_EQ(blake2b_256(""), fixture("blake2b-256-empty.hex"));
  EXPECT_EQ(blake2b_256("abc"), fixture("blake2b-256-abc.hex"));
}

TEST(GoldenCanonicalJson, Rfc8785FloatFreeExample) {
  const Json value(Object{{"string", Json("€$\x0f\nA'B\"\\\"/")},
                          {"literals", Json(Array{Json(nullptr), Json(true), Json(false)})}});
  const std::string bytes = canonical(value);
  EXPECT_EQ(hex(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), fixture("rfc8785-float-free.hex"));
}

TEST(GoldenBatch, ProjectSignedBatch) {
  const Json records(Array{
      Json(Object{{"s", Json(std::int64_t{1})}, {"h", Json(std::int64_t{115344015360000001})},
                   {"t", Json("ADD_NODE")}, {"p", Json(Object{{"layer", Json(std::int64_t{-7})},
                   {"title", Json("Café العربية \"quote\" \\ slash\n")}})}}),
      Json(Object{{"s", Json(std::int64_t{2})}, {"h", Json(std::int64_t{115344015360000002})},
                   {"t", Json("SETTLE")}, {"p", Json(Object{{"kept", Json(std::int64_t{1})},
                   {"end", Json("children")}})}}),
  });
  const std::string canonical_records = canonical(records);
  const auto seed = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  EXPECT_EQ(hex(reinterpret_cast<const std::uint8_t*>(canonical_records.data()), canonical_records.size()),
            fixture("project-batch-records.hex"));
  EXPECT_EQ(blake2b_256(canonical_records), fixture("project-batch-hash.hex"));
  EXPECT_EQ(sign(seed, canonical_records), fixture("project-batch-signature.hex"));
}
}  // namespace
