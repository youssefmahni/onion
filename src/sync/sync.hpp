#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "log/log.hpp"
#include "state/state.hpp"

struct httplib_forward_request;  // never defined; used only via lambdas

namespace onion::sync {
inline constexpr std::string_view component_name{"sync"};

// Signed sync requests (DESIGN › Sync protocol › Request auth): an Ed25519
// signature over device ‖ method ‖ path ‖ body-hash ‖ nonce ‖ timestamp,
// valid for ±60 s, with a per-key nonce cache so a replayed nonce in the
// window is dropped. The nonce is a fresh random hex value PER REQUEST
// ATTEMPT (not per round): a legitimate transport retry signs a new nonce,
// while a replayed request carries a seen nonce and is rejected.
std::string sign_request(std::string_view device_id, std::string_view method,
                         std::string_view path, std::string_view body,
                         std::string_view nonce, std::int64_t timestamp_s,
                         log::Signer& signer);

// Pure signature check (no cache): the bytes signed are the canonical
// concatenation device '\n' method '\n' path '\n' body_hash '\n'
// nonce '\n' timestamp.
bool request_signature_valid(
    std::string_view device_id, std::string_view method,
    std::string_view path, std::string_view body, std::string_view nonce,
    std::string_view signature, const std::vector<std::uint8_t>& public_key,
    std::int64_t timestamp_s, std::int64_t now_s);

// Sliding nonce cache: a (device_id, nonce) pair is accepted once per
// 60-second window; older entries are pruned.
class NonceCache {
 public:
  bool seen_and_add(std::string_view device_id, std::string_view nonce,
                    std::int64_t now_s);

 private:
  struct Entry {
    std::int64_t window{};
  };
  std::map<std::string, Entry> seen_;
};

struct MemberKey {
  std::string device_id;
  std::vector<std::uint8_t> public_key;
};

// M7 joining: clone the peer's newest checkpoint into `destination` (the
// joiner's data dir): fetch the manifest, verify its signature against
// the peer's key, download the snapshot, verify its hash and its Merkle
// root, and install it as onion.db. Returns "" on success or the reason.
struct CloneOptions {
  std::string device_id;              // the JOINING device's id
  log::Ed25519Signer& signer;         // the joining device's key
  std::map<std::string, MemberKey> members;  // the known fleet (device -> key)
  std::filesystem::path destination;  // data dir receiving onion.db
};
std::string clone_checkpoint(std::string_view peer_base_url,
                             const CloneOptions& options);

// HTTP sync server on TCP 7947 (DESIGN › Sync protocol). Every endpoint
// authenticates the caller with the signed-request scheme.
class SyncServer {
 public:
  struct Options {
    int port{7947};
    std::string bind_address{"0.0.0.0"};  // the LAN interface; loopback in tests
    state::StateWriter* writer{};
    std::shared_ptr<const log::DeviceKey> key;
    std::vector<MemberKey> members;  // device_id -> public key
  };

  explicit SyncServer(Options options);
  ~SyncServer();
  SyncServer(const SyncServer&) = delete;
  SyncServer& operator=(const SyncServer&) = delete;

  bool start(std::string* error);
  void stop();
  int bound_port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend struct Impl;
};

// The pull side of one sync round (DESIGN › Sync protocol › Round): compare
// vectors, fetch missing per-origin ranges, verify each batch, quarantine
// failures, and hand verified batches to the state writer.
class SyncClient {
 public:
  struct Options {
    state::StateWriter& writer;
    std::shared_ptr<const log::DeviceKey> key;
    std::map<std::string, MemberKey> members;  // by device id
  };

  explicit SyncClient(Options options);
  ~SyncClient();

  // One round against one peer: returns the number of batches applied.
  // Quarantined batches are reported through `quarantined` (path:reason).
  int run_round(std::string_view peer_base_url,
                std::map<std::string, std::string>* quarantined,
                std::string* error);
  // M7 joining: how far this device lags the peer — the maximum over all
  // origins of (peer's last seq - our applied seq). -1 on any error.
  std::int64_t peer_lag(std::string_view peer_base_url, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onion::sync
