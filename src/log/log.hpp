#pragma once

#include <array>
#include <cstdint>
#include <optional>
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

// Ed25519 device identity (Monocypher). The keypair is generated from a
// 32-byte seed; the device_id is the first 16 hex chars of BLAKE2b-256 of
// the public key, and the slot is its top 20 bits (slot 0 = legacy ids).
struct DeviceKey {
  std::vector<std::uint8_t> seed;    // 32 bytes
  std::vector<std::uint8_t> public_key;  // 32 bytes
  std::string device_id;             // 16 hex chars
  std::int64_t slot{};               // 0..2^20-1

  static DeviceKey generate();                          // random seed
  static DeviceKey from_seed(const std::vector<std::uint8_t>& seed);
  static DeviceKey from_seed_hex(std::string_view seed_hex);
  std::string seed_hex() const;
};

class Ed25519Signer final : public Signer {
 public:
  explicit Ed25519Signer(const DeviceKey& key);
  std::string sign(std::string_view bytes) override;

 private:
  std::array<std::uint8_t, 64> secret_{};  // Monocypher secret: seed ‖ pubkey
  std::vector<std::uint8_t> public_key_;
};

// The previous-hash value of batch 1: BLAKE2b-256 of the public key.
std::string genesis_prev(const DeviceKey& key);

// Hex <-> bytes helpers (shared by the log, invite and state folds).
std::vector<std::uint8_t> unhex_bytes(std::string_view hex);
std::string hex_bytes(const std::vector<std::uint8_t>& raw);

// A device's derived identity from a public key (64 hex): the device id
// (first 16 hex of the key digest) and its 20-bit slot.
struct DerivedIdentity {
  std::string device_id;
  std::int64_t slot{};
};
std::optional<DerivedIdentity> derive_identity(std::string_view pubkey_hex);

// Verification outcome per DESIGN.md › Op log format › Verification on
// receipt. Ok means the batch may be applied; every other value names the
// rejection and the batch must be quarantined.
enum class Verify {
  Ok,
  BadSig,
  BadPrev,
  BadHash,
  Gap,
  NotMember,
  PastCutoff,
  BadVersion,
};

struct VerifyContext {
  int version{1};                       // the version this build applies
  std::string expected_prev;            // stored hash of the previous batch
  std::int64_t expected_first_seq{};    // next contiguous record number
  const std::vector<std::string>* members{};  // active device_ids (null = skip)
  std::int64_t evict_cutoff{};          // per-origin cutoff, 0 = none
  // The origin's public key (32 bytes). In M4 the single device verifies
  // against its own key; M6 passes the member's key.
  const std::vector<std::uint8_t>* origin_public_key{};
};

Verify verify(const Batch& batch, const VerifyContext& context);

// A MEMBER_EVICT record is valid when it carries the offline admin key's
// signature over its payload, or signatures from a majority of members.
// Each signature is an Ed25519 signature over the canonical payload JSON
// with the "sigs" key removed.
bool eviction_valid(const Record& record,
                    const std::vector<std::string>& member_public_keys_hex,
                    std::string_view admin_public_key_hex);

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
  // segment when the write would cross seal_bytes; the closed segment is
  // sealed: zstd-compressed to <first_seq>.log.zst and never modified again.
  bool append(const Batch& batch, std::string* error);
  // Seals the active segment (zstd) without rotating; used at shutdown so
  // a 64 MB-flush never loses a partial segment to a crash. Returns true
  // when a seal happened.
  bool seal_active(std::string* error);
  // Releases the open file handle so the directory can be removed (Windows
  // keeps open files locked).
  void close();

  std::uint64_t active_first_seq() const;
  std::uint64_t total_batches() const;
  std::filesystem::path active_path() const;
  std::uint64_t active_bytes() const;
  // Sealed segments, in first_seq order, with their zstd paths.
  std::vector<std::uint64_t> sealed_first_seqs() const;
  std::filesystem::path sealed_path(std::uint64_t first_seq) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onion::log
