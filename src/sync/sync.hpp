#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

// A fresh request nonce: 32 lowercase hex characters (128 bits) from the
// platform random device, one value per request attempt (decision 0018).
std::string fresh_nonce();

// Signed POST headers (M7 evict submission): the same scheme with a
// fresh random nonce per attempt, covering the request body. Returned as
// a plain map so this header needs no httplib include.
std::map<std::string, std::string> signed_post_headers(
    std::string_view device_id, std::string_view path,
    std::string_view body, log::Signer& signer);

// Sliding nonce cache: a (device_id, nonce) pair is accepted once per
// 60-second window; older entries are pruned. Thread-safe: the server's
// handler threads share one cache.
class NonceCache {
 public:
  bool seen_and_add(std::string_view device_id, std::string_view nonce,
                    std::int64_t now_s);

 private:
  struct Entry {
    std::int64_t window{};
  };
  std::mutex mutex_;
  std::map<std::string, Entry> seen_;
};

// Decision 0014 (amendment 2026-10-13): a sync round is COMPLETE only
// when a MAJORITY of members (counting self) answered within their
// timeouts. `answered_peers` excludes self; the rule adds it:
//   (answered_peers + 1) * 2 > total_members
// A 1-member fleet therefore completes every round (0/1 is a majority),
// and a 2-member fleet needs BOTH devices — a slow peer there pauses
// reservations (documented consequence; the admin-key eviction shrinks
// a stuck 2-fleet to 1). Pure and deterministic, so replicas that see
// the same answers make the same verdict.
inline bool majority_round(std::size_t answered_peers,
                           std::size_t total_members) {
  return (answered_peers + 1) * 2 > total_members;
}

// The devices that no longer take part in the fleet: members-table rows in
// state EVICTED or LEFT. A round never contacts them and never counts them.
std::set<std::string> retired_member_ids(
    const std::vector<state::StateWriter::MemberRow>& table);

// The denominator of majority_round(): every device that is a CURRENT
// member — folded table rows not retired, plus the static members.json ids
// (static fleets never write MEMBER_ADD) that the table has not retired —
// and this device itself, which always answers. members.json is never
// pruned, so an evicted or departed device listed there must not keep
// inflating the denominator (decision 0014: a stuck 2-fleet member is
// removed by eviction and the survivor then completes rounds alone).
std::size_t majority_denominator(
    const std::vector<std::string>& listed_ids,
    const std::vector<state::StateWriter::MemberRow>& table,
    std::string_view self_id);

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
    // M7 joining: the fleet's reachable addresses, so the admission
    // response can tell the joiner where every member syncs and gossips.
    std::map<std::string, std::string> member_urls;
    std::string self_url;  // this device's own host:port
    std::map<std::string, std::string> member_swim_urls;
    std::string self_swim_url;  // this device's own host:udp_port
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
    // M7: this device's own sync address, carried in the push headers so
    // the receiver learns where to pull from.
    std::string self_url;
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
  // M7: push one batch (JSON as written in the log) to the peer.
  void push_batch(std::string_view peer_base_url,
                  std::string_view batch_json, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onion::sync
