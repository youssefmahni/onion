#include "sync.hpp"

#include "log/invite.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

extern "C" {
#include <monocypher.h>
#include <monocypher-ed25519.h>
}

#include "store/checkpoint.hpp"

namespace onion::sync {

namespace {

std::string signed_bytes(std::string_view device_id, std::string_view method,
                         std::string_view path, std::string_view body,
                         std::string_view nonce,
                         std::int64_t timestamp_s) {
  std::ostringstream out;
  out << device_id << '\n'
      << method << '\n'
      << path << '\n'
      << onion::log::blake2b_hex(body) << '\n'
      << nonce << '\n'
      << timestamp_s;
  return out.str();
}

std::int64_t now_s() {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// The exact signed bytes, hex-encoded, for the OA-13-style diagnosis of a
// rejected request: compare Linux vs macOS byte for byte.
std::string signed_bytes_hex(std::string_view device_id,
                             std::string_view method, std::string_view path,
                             std::string_view body, std::string_view nonce,
                             std::int64_t timestamp_s) {
  const std::string bytes =
      signed_bytes(device_id, method, path, body, nonce, timestamp_s);
  static const char* digits = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 0xf]);
  }
  return hex;
}

}  // namespace

std::string sign_request(std::string_view device_id, std::string_view method,
                         std::string_view path, std::string_view body,
                         std::string_view nonce, std::int64_t timestamp_s,
                         log::Signer& signer) {
  return signer.sign(
      signed_bytes(device_id, method, path, body, nonce, timestamp_s));
}

bool request_signature_valid(
    std::string_view device_id, std::string_view method,
    std::string_view path, std::string_view body, std::string_view nonce,
    std::string_view signature, const std::vector<std::uint8_t>& public_key,
    std::int64_t timestamp_s, std::int64_t now_s) {
  if (public_key.size() != 32) return false;
  if (nonce.empty() || nonce.size() > 64) return false;
  if (timestamp_s < now_s - 60 || timestamp_s > now_s + 60) return false;
  const std::string message =
      signed_bytes(device_id, method, path, body, nonce, timestamp_s);
  const std::vector<std::uint8_t> signature_bytes =
      [] (std::string_view hex) {
        std::vector<std::uint8_t> raw;
        raw.reserve(hex.size() / 2);
        const auto digit = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          return -1;
        };
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
          const int hi = digit(hex[i]);
          const int lo = digit(hex[i + 1]);
          if (hi < 0 || lo < 0) return std::vector<std::uint8_t>{};
          raw.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        }
        return raw;
      }(signature);
  if (signature_bytes.size() != 64) return false;
  return crypto_ed25519_check(signature_bytes.data(), public_key.data(),
                              reinterpret_cast<const std::uint8_t*>(message.data()),
                              message.size()) == 0;
}

bool NonceCache::seen_and_add(std::string_view device_id,
                              std::string_view nonce, std::int64_t now_s) {
  const std::string key = std::string(device_id) + ":" + std::string(nonce);
  const std::int64_t window = now_s / 60;
  // Prune old windows.
  for (auto it = seen_.begin(); it != seen_.end();)
    if (it->second.window < window - 1)
      it = seen_.erase(it);
    else
      ++it;
  auto found = seen_.find(key);
  if (found != seen_.end() && found->second.window >= window - 1) return true;
  seen_[key] = Entry{window};
  return false;
}

// ----------------------------------------------------------------- server

struct SyncServer::Impl {
  Options options;
  std::unique_ptr<httplib::Server> server;
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<int> port{0};
  NonceCache nonces;
  std::function<const MemberKey*(const httplib::Request&, httplib::Response&)>
      authenticate;
};

SyncServer::SyncServer(Options options)
    : impl_(new Impl{std::move(options), nullptr, {}, false, 0, {}}) {}
SyncServer::~SyncServer() { stop(); }

namespace {

const MemberKey* find_member(const std::vector<MemberKey>& members,
                             std::string_view device_id) {
  for (const MemberKey& member : members)
    if (member.device_id == device_id) return &member;
  return nullptr;
}

}  // namespace

bool SyncServer::start(std::string* error) {
  impl_->server = std::make_unique<httplib::Server>();
  // The join endpoint is unauthenticated (the token IS the credential):
  // cap request bodies so it cannot be used as a cheap amplifier.
  impl_->server->set_payload_max_length(1 << 16);

  // Authentication for every endpoint. The handler lambdas must capture
  // `this` (not stack locals of start(), which dangle once start returns).
  impl_->authenticate = [this](const httplib::Request& request,
                                httplib::Response& response) -> const MemberKey* {
    const std::string device_id = request.get_header_value("X-Onion-Device");
    const std::string timestamp = request.get_header_value("X-Onion-Time");
    const std::string signature = request.get_header_value("X-Onion-Sig");
    const std::string nonce = request.get_header_value("X-Onion-Nonce");
    const auto reject = [&](const char* reason) -> const MemberKey* {
      if (std::getenv("ONION_SYNC_DEBUG"))
        std::fprintf(stderr,
                     "[sync-auth] REJECT %s: device=%s ts-header=%s "
                     "sig-header=%s nonce-header=%s target=%s body-hash=%s "
                     "now=%lld signed-bytes-hex=%s\n",
                     reason, device_id.c_str(), timestamp.c_str(),
                     signature.c_str(), nonce.c_str(),
                     request.target.c_str(),
                     onion::log::blake2b_hex(request.body).c_str(),
                     static_cast<long long>(now_s()),
                     signed_bytes_hex(device_id, "GET", request.target,
                                      request.body, nonce,
                                      std::atoll(timestamp.c_str()))
                         .c_str());
      response.status = 401;
      response.set_content(std::string("{\"error\":\"") + reason +
                               "\"}",
                           "application/json");
      return nullptr;
    };
    if (device_id.empty() || timestamp.empty() || signature.empty() ||
        nonce.empty())
      return reject("missing auth header");
    const MemberKey* member = find_member(impl_->options.members, device_id);
    if (!member) return reject("unknown device");
    const std::int64_t ts = std::atoll(timestamp.c_str());
    // The signature covers the full target (path + query); httplib keeps
    // the query only in request.target.
    if (!request_signature_valid(device_id, "GET", request.target, request.body,
                                 nonce, signature, member->public_key, ts,
                                 now_s()))
      return reject("bad signature");
    if (impl_->nonces.seen_and_add(device_id, nonce, now_s()))
      return reject("replayed nonce");
    return member;
  };

  // M7 joining: the invitation token is the credential (it is signed by a
  // member); a valid, unused token with a free slot admits the device via
  // a MEMBER_ADD record in the server's own log.
  impl_->server->Post("/sync/v1/join", [this](const httplib::Request& request,
                                               httplib::Response& response) {
    const nlohmann::json token_json =
        nlohmann::json::parse(request.body, nullptr, false);
    const auto reject = [&](int status, std::string reason) {
      response.status = status;
      response.set_content(
          nlohmann::json{{"error", std::move(reason)}}.dump(),
          "application/json");
    };
    if (token_json.is_discarded() || !token_json.contains("inviter") ||
        !token_json["inviter"].is_string())
      return reject(400, "malformed token");
    const std::string inviter = token_json["inviter"].get<std::string>();
    const MemberKey* inviter_key = find_member(impl_->options.members, inviter);
    if (!inviter_key) return reject(401, "unknown inviter");
    std::string token_error;
    const auto parsed = log::parse_invite(
        request.body, now_s(), inviter_key->public_key, &token_error);
    if (!parsed) return reject(401, token_error);
    if (impl_->options.writer->join_nonce_seen(parsed->nonce))
      return reject(409, "token already used");
    const auto identity = log::derive_identity(parsed->pubkey_hex);
    if (!identity) return reject(400, "bad public key");
    if (impl_->options.writer->member_slot_collides(identity->slot,
                                                    identity->device_id))
      return reject(409, "slot collision");
    log::Record admission;
    admission.type = "MEMBER_ADD";
    admission.payload = {{"pubkey", parsed->pubkey_hex},
                         {"nonce", parsed->nonce},
                         {"invited_by", inviter}};
    impl_->options.writer->submit(
        state::CommitTerminal{{std::move(admission)}});
    impl_->options.writer->sync();
    // The fold is the authority: sync() drained it. Presence in the
    // members table means OUR add (or an earlier add of the same device)
    // won; absence means a concurrent same-slot admission beat us.
    if (!impl_->options.writer->member_exists(identity->device_id))
      return reject(409, "slot collision");
    response.status = 200;
    response.set_content(
        nlohmann::json{{"ok", true},
                       {"device_id", identity->device_id},
                       {"slot", identity->slot}}
            .dump(),
        "application/json");
  });

  impl_->server->Get("/sync/v1/vector", [this](const httplib::Request& request,
                                                httplib::Response& response) {
    if (!impl_->authenticate(request, response)) return;
    auto& writer = *impl_->options.writer;
    nlohmann::json json;
    json["device_id"] = impl_->options.key->device_id;
    json["incarnation"] = 1;
    for (const auto& [origin, seq] : writer.vector())
      json["vector"][origin] = seq;
    response.set_content(json.dump(), "application/json");
  });

  impl_->server->Get("/sync/v1/log/(.*)",
                     [this](const httplib::Request& request,
                            httplib::Response& response) {
    if (!impl_->authenticate(request, response)) return;
    auto& writer = *impl_->options.writer;
    const std::string origin = request.matches[1];
    std::int64_t from = request.has_param("from")
                            ? std::atoll(request.get_param_value("from").c_str())
                            : 1;
    const std::int64_t max_bytes = request.has_param("max_bytes")
        ? std::atoll(request.get_param_value("max_bytes").c_str())
        : 8388608;
    // Serve batches from the origin's active segment (sealed segments are
    // fetched whole via /segment).
    const std::filesystem::path dir =
        [&]() {
          if (origin == impl_->options.key->device_id)
            return writer.segments().active_path().parent_path();
          return writer.segments().active_path().parent_path().parent_path() /
                 origin;
        }();
    std::ostringstream body;
    std::error_code ec;
    if (std::filesystem::exists(dir)) {
      std::vector<std::filesystem::path> files;
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".log") files.push_back(entry.path());
      }
      std::sort(files.begin(), files.end());
      for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
          if (line.empty()) continue;
          auto batch = nlohmann::json::parse(line, nullptr, false);
          if (batch.is_discarded() || !batch.contains("recs")) {
            // Serve the raw line: the client's verification quarantines a
            // corrupt batch; silently dropping it would hide corruption.
            body << line << '\n';
            continue;
          }
          std::int64_t first = batch["recs"].empty()
                                   ? 0
                                   : batch["recs"][0]["s"].get<std::int64_t>();
          std::int64_t last = batch["recs"].empty()
                                  ? first
                                  : batch["recs"].back()["s"].get<std::int64_t>();
          if (last < from) continue;
          if (first < from) {
            // A batch spanning the boundary: serve the whole batch (the
            // client dedups by verification).
          }
          if (static_cast<std::int64_t>(body.tellp()) +
                  static_cast<std::int64_t>(line.size()) + 1 >
              max_bytes)
            break;
          body << line << '\n';
        }
      }
    }
    response.set_content(body.str(), "application/x-ndjson");
  });

  impl_->server->Get("/sync/v1/segment/([^/]+)/([^/]+)",
                     [this](const httplib::Request& request,
                            httplib::Response& response) {
    if (!impl_->authenticate(request, response)) return;
    auto& writer = *impl_->options.writer;
    const std::string origin = request.matches[1];
    const std::string first = request.matches[2];
    const std::filesystem::path base =
        origin == impl_->options.key->device_id
            ? writer.segments().active_path().parent_path()
            : writer.segments().active_path().parent_path().parent_path() /
                  origin;
    const std::filesystem::path sealed =
        base / (first + ".log.zst");
    const std::filesystem::path raw = base / (first + ".log");
    std::ifstream in(sealed, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) {
      std::ifstream raw_in(raw, std::ios::binary);
      content.assign((std::istreambuf_iterator<char>(raw_in)),
                     std::istreambuf_iterator<char>());
      response.set_content(content, "application/x-ndjson");
    } else {
      response.set_content(content, "application/octet-stream");
      response.set_header("X-Onion-Sealed", "1");
    }
  });

  impl_->server->Get("/sync/v1/merkle/([^/]+)",
                     [this](const httplib::Request& request,
                            httplib::Response& response) {
    if (!impl_->authenticate(request, response)) return;
    auto& writer = *impl_->options.writer;
    const std::string origin = request.matches[1];
    std::string error;
    const std::vector<store::Node> nodes = writer.db().all_nodes(&error);
    nlohmann::json json;
    json["origin"] = origin;
    json["root"] = store::merkle_root(nodes);
    response.set_content(json.dump(), "application/json");
  });

  impl_->running.store(true);
  impl_->thread = std::thread([this] {
    if (impl_->options.port == 0) {
      const int bound = impl_->server->bind_to_any_port(impl_->options.bind_address);
      impl_->port.store(bound);
      if (bound < 0) impl_->running.store(false);
    } else {
      impl_->port.store(impl_->options.port);
      if (!impl_->server->bind_to_port(impl_->options.bind_address,
                                       impl_->options.port))
        impl_->running.store(false);
    }
    while (impl_->running.load())
      if (!impl_->server->listen_after_bind()) break;
  });
  for (int i = 0; i < 200 && impl_->port.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (impl_->port.load() <= 0) {
    if (error) *error = "sync server failed to bind";
    impl_->running.store(false);
    if (impl_->thread.joinable()) impl_->thread.join();
    return false;
  }
  return true;
}

void SyncServer::stop() {
  if (!impl_->running.load() && !impl_->thread.joinable()) return;
  impl_->running.store(false);
  if (impl_->server) impl_->server->stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

int SyncServer::bound_port() const { return impl_->port.load(); }

// ----------------------------------------------------------------- client

struct SyncClient::Impl {
  Options options;
};

SyncClient::SyncClient(Options options)
    : impl_(new Impl{std::move(options)}) {}
SyncClient::~SyncClient() = default;

int SyncClient::run_round(std::string_view peer_base_url,
                          std::map<std::string, std::string>* quarantined,
                          std::string* error) {
  int applied = 0;
  httplib::Client client(std::string(peer_base_url).c_str());
  // A stalled peer must never block the node loop: bounded timeouts.
  client.set_connection_timeout(3, 0);
  client.set_read_timeout(5, 0);
  client.set_write_timeout(5, 0);

  // 1. Compare vectors.
  const std::string self = impl_->options.key->device_id;
  const auto self_vector = impl_->options.writer.vector();
  const std::int64_t ts = now_s();
  log::Ed25519Signer signer(*impl_->options.key);
  auto request_headers = [&](std::string_view path) {
    // A fresh random nonce per ATTEMPT: a transport retry signs a new
    // nonce, so the anti-replay cache never rejects a legitimate retry
    // (the macOS 401 root cause: httplib retried with the same bytes).
    std::random_device entropy;
    std::mt19937_64 generator(entropy());
    std::uniform_int_distribution<std::uint64_t> distribution;
    std::ostringstream nonce_stream;
    nonce_stream << std::hex << distribution(generator)
                 << distribution(generator);
    const std::string nonce = nonce_stream.str();
    const std::string signature =
        sign_request(self, "GET", path, "", nonce, ts, signer);
    if (std::getenv("ONION_SYNC_DEBUG"))
      std::fprintf(stderr,
                   "[sync-client] device=%s path=%s ts=%lld nonce=%s "
                   "body-hash=%s sig=%s signed-bytes-hex=%s\n",
                   self.c_str(), std::string(path).c_str(),
                   static_cast<long long>(ts), nonce.c_str(),
                   onion::log::blake2b_hex("").c_str(), signature.c_str(),
                   signed_bytes_hex(self, "GET", path, "", nonce, ts)
                       .c_str());
    return httplib::Headers{{"X-Onion-Device", self},
                            {"X-Onion-Time", std::to_string(ts)},
                            {"X-Onion-Nonce", nonce},
                            {"X-Onion-Sig", signature}};
  };
  auto response = client.Get("/sync/v1/vector",
                             request_headers("/sync/v1/vector"));
  if (!response || response->status != 200) {
    if (error) {
      std::ostringstream out;
      out << "vector request failed (status "
          << (response ? response->status : -1) << " "
          << (response ? response->body : "") << ")";
      *error = out.str();
    }
    return applied;
  }
  auto peer_vector_json = nlohmann::json::parse(response->body, nullptr, false);
  if (peer_vector_json.is_discarded()) {
    if (error) *error = "malformed vector response";
    return applied;
  }

  if (std::getenv("ONION_SYNC_DEBUG")) std::fprintf(stderr, "sync: vector ok, pulling\n");
  // 2. Pull every origin where the peer is ahead.
  const std::string peer_id =
      peer_vector_json.value("device_id", std::string{});
  if (peer_id.empty() || !impl_->options.members.count(peer_id)) {
    if (error) *error = "unknown peer identity";
    return applied;
  }
  const auto& peer_key = impl_->options.members.at(peer_id);
  for (const auto& [origin, peer_seq] :
       peer_vector_json["vector"].items()) {
    const std::int64_t peer_last = peer_seq.get<std::int64_t>();
    const auto local = self_vector.find(origin);
    const std::int64_t local_last =
        local == self_vector.end() ? 0 : local->second;
    if (peer_last <= local_last) continue;

    if (std::getenv("ONION_SYNC_DEBUG")) std::fprintf(stderr, "sync: pulling %s from %lld\n", origin.c_str(), (long long)(local_last + 1));
    // 3. Fetch batches from local_last + 1.
    std::string path = "/sync/v1/log/" + origin + "?from=" +
                       std::to_string(local_last + 1);
    auto log_response = client.Get(path.c_str(), request_headers(path));
    if (!log_response || log_response->status != 200) {
      if (quarantined) (*quarantined)[origin] = "log fetch failed";
      continue;
    }

    // 4. Verify and append each batch, continuing the origin's LOCAL
    //    chain (a mid-chain pull must link to the last batch we already
    //    hold — starting empty only works for batch 1).
    std::istringstream in(log_response->body);
    std::string line;
    std::string expected_prev =
        impl_->options.writer.origin_prev_hash(origin).value_or("");
    std::int64_t expected_first = local_last + 1;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      auto batch_json = nlohmann::json::parse(line, nullptr, false);
      if (batch_json.is_discarded() || !batch_json.contains("recs")) {
        if (quarantined) (*quarantined)[origin] = "unparseable batch";
        break;
      }
      log::Batch batch;
      batch.version = batch_json.value("v", 0);
      batch.origin = batch_json.value("o", std::string{});
      batch.number = batch_json.value("b", std::uint64_t{0});
      batch.prev = batch_json.value("prev", std::string{});
      batch.hash = batch_json.value("hash", std::string{});
      batch.sig = batch_json.value("sig", std::string{});
      for (const auto& record_json : batch_json["recs"])
        batch.records.push_back(log::Record::from_json(record_json));

      log::VerifyContext context;
      context.version = 1;
      context.expected_prev = expected_prev;
      context.expected_first_seq = expected_first;
      context.origin_public_key = &peer_key.public_key;
      // The first batch of an origin chains from that origin's public-key
      // hash (the batch's own origin may be a third device).
      if (expected_prev.empty() && batch.number == 1) {
        const std::string origin_pubkey =
            [&]() -> std::string {
              if (batch.origin == peer_id)
                return onion::log::blake2b_hex(std::string_view(
                    reinterpret_cast<const char*>(peer_key.public_key.data()),
                    peer_key.public_key.size()));
              const auto member = impl_->options.members.find(batch.origin);
              if (member == impl_->options.members.end()) return {};
              const auto& key_bytes = member->second.public_key;
              return onion::log::blake2b_hex(std::string_view(
                  reinterpret_cast<const char*>(key_bytes.data()),
                  key_bytes.size()));
            }();
        if (origin_pubkey.empty()) {
          if (quarantined) (*quarantined)[origin] = "unknown origin key";
          break;
        }
        context.expected_prev = origin_pubkey;
      }
      const log::Verify verified = log::verify(batch, context);
      if (verified != log::Verify::Ok) {
        if (quarantined)
          (*quarantined)[origin] = "verify " +
                                   std::to_string(static_cast<int>(verified));
        break;
      }
      expected_prev = batch.hash;
      expected_first =
          batch.records.empty()
              ? expected_first
              : static_cast<std::int64_t>(batch.records.back().seq) + 1;

      impl_->options.writer.submit(state::AppendForeign{std::move(batch)});
      applied++;
    }
    if (std::getenv("ONION_SYNC_DEBUG")) std::fprintf(stderr, "sync: syncing foreign batches\n");
    impl_->options.writer.sync();
    if (std::getenv("ONION_SYNC_DEBUG")) std::fprintf(stderr, "sync: foreign batches folded\n");
  }
  return applied;
}

}  // namespace onion::sync
