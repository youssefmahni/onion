#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace onion::log {
inline constexpr std::string_view component_name{"log"};

using Hlc = std::uint64_t;

// RFC 8785 subset used for records: object keys sorted, no insignificant
// whitespace, minimal string escaping, integers as exact signed 64-bit
// decimal (decision 0003). Floating-point values are forbidden and make it
// throw, because records must never contain floats.
std::string canonical_json(const nlohmann::json& value);

// BLAKE2b-256 of the given bytes, lowercase hex.
std::string blake2b_hex(std::string_view bytes);

struct Record {
  std::int64_t seq{};
  Hlc hlc{};
  std::string type;
  nlohmann::json payload = nlohmann::json::object();

  nlohmann::json to_json() const;
  static Record from_json(const nlohmann::json& value);
};

// Canonical JSON of the records array ({"s":..,"h":..,"t":..,"p":..}).
std::string records_json(const std::vector<Record>& records);

// BLAKE2b-256 over the canonical bytes of the records, lowercase hex.
std::string hash_records_hex(const std::vector<Record>& records);

struct Batch {
  int version{1};
  std::string origin;      // device_id, hex
  std::uint64_t number{};
  std::string prev;        // hex hash of the previous batch
  std::string hash;        // hex hash of the records
  std::string sig;         // hex signature over prev ‖ hash
  std::vector<Record> records;

  nlohmann::json to_json() const;
};

// Signs the 64 raw bytes prev ‖ hash and returns a lowercase hex signature.
class Signer {
 public:
  virtual ~Signer() = default;
  virtual std::string sign(std::string_view bytes) = 0;
};

// M3 signer: the signature is empty; M4 replaces it with Ed25519.
class NullSigner final : public Signer {
 public:
  std::string sign(std::string_view) override { return {}; }
};

// Builds a batch: canonicalizes the records, hashes them, signs prev ‖ hash.
Batch make_batch(std::string origin, std::uint64_t number,
                 std::string prev_hex, std::vector<Record> records,
                 Signer& signer);

// Hybrid logical clock: (physical_ms << 16) | logical counter, monotonic per
// device (decision 0003: exact signed 64-bit decimal in records).
class HlcClock {
 public:
  Hlc now();

 private:
  Hlc last_{};
};

// Append-only per-origin log segments: one canonical JSON line per batch,
// LF only, one durable write per append. Seals at 64 MB (a new segment
// starts; zstd compression of sealed segments arrives in M4).
class Segments {
 public:
  struct Options {
    std::filesystem::path dir;    // oplog/<origin>
    std::string origin{};         // for diagnostics
    std::uint64_t seal_bytes{64u * 1024u * 1024u};
  };

  explicit Segments(Options options);
  ~Segments();
  Segments(const Segments&) = delete;
  Segments& operator=(const Segments&) = delete;
  Segments(Segments&& other) noexcept;
  Segments& operator=(Segments&& other) noexcept;

  // Creates the directory and re-opens the newest segment. A trailing line
  // without a final LF is a torn tail and is cut. (Full hash/signature
  // repair of the torn tail arrives in M4.)
  bool open(std::string* error);

  // Appends one batch as one JSON line and durably syncs. Rotates to a new
  // segment when the write would cross seal_bytes.
  bool append(const Batch& batch, std::string* error);

  std::uint64_t active_first_seq() const;
  std::uint64_t total_batches() const;
  std::filesystem::path active_path() const;
  std::uint64_t active_bytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onion::log
