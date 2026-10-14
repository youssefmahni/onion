#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "store.hpp"

namespace onion::store {

// Merkle root over nodes sorted by path (decision 0009): each leaf is
// BLAKE2b-256 of the canonical JSON of {code, path, layer, title, hlc,
// tombstone}; internal nodes hash their children's digests.
std::string merkle_root(const std::vector<Node>& nodes);

struct CheckpointManifest {
  std::uint64_t hlc{};
  std::string snapshot_hash;   // BLAKE2b-256 of the snapshot file bytes
  std::string merkle_root;
  std::map<std::string, std::int64_t> vector;  // origin -> last_seq
  // M8 compaction metadata.  The checkpoint snapshot contains per-path
  // summaries for these old audit records instead of their raw rows.
  std::uint64_t compacted_before_hlc{};
  std::uint64_t compacted_attempts{};
  std::uint64_t compacted_critiques{};
  std::uint64_t compacted_nodes{};
  std::string signature;       // the creating device (hex)
  std::string cosignature;     // a second device (empty until M6)

  std::string to_json() const;
};

// Writes one checkpoint: VACUUM INTO checkpoint-<hlc>.db, the manifest next
// to it, and prunes so only the last 3 pairs remain. The manifest is signed
// with the provided signer over the canonical manifest bytes.
class CheckpointWriter {
 public:
  // signer signs the canonical manifest JSON (signature/cosignature empty).
  CheckpointWriter(std::filesystem::path dir,
                   std::function<std::string(std::string_view)> signer);
  bool write(std::uint64_t hlc,
             const std::map<std::string, std::int64_t>& vector,
             std::string* error);

 private:
  std::filesystem::path dir_;
  std::function<std::string(std::string_view)> signer_;
};

}  // namespace onion::store
