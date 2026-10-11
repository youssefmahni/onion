#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "swim/membership.hpp"

namespace onion::swim {
inline constexpr std::string_view component_name{"swim"};

// The SWIM protocol engine (DESIGN › Membership): failure detection by
// ping → 2 indirect probes → SUSPECT → refutation (incarnation + 1) →
// DEAD after the suspicion timeout, with membership updates piggybacked on
// ping and ack messages. The transport and the clock are injected so the
// whole protocol runs deterministically in tests.

class Clock {
 public:
  virtual ~Clock() = default;
  virtual std::int64_t now_ms() const = 0;
};

// A monotonic clock based on steady_clock; milliseconds since an arbitrary
// epoch, used only for intervals.
class SteadyClock final : public Clock {
 public:
  std::int64_t now_ms() const override;
};

// Deterministic clock for tests: starts at `start_ms` and only moves when
// the test advances it.
class FakeClock final : public Clock {
 public:
  explicit FakeClock(std::int64_t start_ms = 0) : now_(start_ms) {}
  std::int64_t now_ms() const override { return now_; }
  void set(std::int64_t ms) { now_ = ms; }
  void advance(std::int64_t ms) { now_ += ms; }

 private:
  std::int64_t now_;
};

// A transport delivers datagrams to an address and hands inbound datagrams
// to a receiver callback. The real UDP socket and the in-memory test bus
// both satisfy this.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual void send(const std::string& address,
                    std::string_view datagram) = 0;
  virtual void set_receiver(
      std::function<void(const std::string& from_address,
                         std::string datagram)> receiver) = 0;
};

struct Options {
  std::string self_id;
  std::string self_address;        // host:port the local device listens on
  std::int64_t period_ms{1000};         // one probe round per second
  std::int64_t ack_timeout_ms{500};     // wait for a direct ack
  std::int64_t indirect_timeout_ms{500};  // wait for indirect probes
  std::int64_t suspicion_timeout_ms{300000};  // 5 min, configurable in tests
  std::int64_t beacon_period_ms{5000};  // LAN beacon every 5 s
  int indirect_probes{2};               // how many members probe indirectly
  int gossip_budget{4};                 // piggyback updates per message
};

// A pending, unsent membership update for piggybacked gossip.
struct Gossip {
  Member member;
  bool force{false};  // suspicion and death updates travel even at budget
};

class Swim {
 public:
  explicit Swim(Options options);

  // Attach the transport: outbound datagrams go through it and inbound
  // datagrams arrive via handle_datagram (a real socket loop does the
  // same, so the protocol stays transport-agnostic).
  void set_transport(Transport* transport);
  const Options& options() const { return options_; }
  // Swapping the clock reschedules the next beacon from the new clock's
  // reading (tests and chaos runs swap in a fake clock after start).
  void set_clock(Clock* clock);

  // Advance the protocol: probe rounds, indirect probes, suspicion expiry
  // and the beacon. Call on a timer (and after each handled datagram).
  void tick();

  // Probe one specific member now (used by join handshakes toward a seed
  // address; tests drive the failure-detection scenarios through it).
  // No-op when the member is unknown or a probe is already in flight.
  void probe_now(const std::string& device_id);

  // Test/chaos seam: run the failure-detection timers (suspicion expiry,
  // forwarded-ping expiry, probe deadlines) without starting new probes
  // or the beacon. tick() runs this first.
  void advance_timers();

  // An inbound datagram from `from_address`.
  void handle_datagram(const std::string& from_address,
                       std::string_view datagram);

  // Membership access for the rest of the node.
  const Membership& membership() const { return membership_; }
  Membership& mutable_membership() { return membership_; }

  // Gossip an update (state change, incarnation, capabilities, vector); it
  // rides on the next outbound message. The local device announces itself
  // by gossiping its own record.
  void gossip(const Member& member, bool force = false);
  void announce();  // gossip the local device's own record
  // Hearing ourselves gossiped as SUSPECT triggers the refutation: ALIVE
  // at max(own, heard) incarnation + 1 (DESIGN step 4). No-op while
  // LEAVING or LEFT.
  void refute_suspicion(std::int64_t heard_incarnation);

  // Graceful leave: LEAVING now, and LEFT once in-flight nodes drain
  // (the drain hook belongs to the caller; this only marks the state).
  void start_leaving();
  void confirm_left();

  // The beacon: when the beacon period elapses, tick() hands the payload to
  // this hook (unsigned here; the platform layer signs it, and the group
  // address comes from a decision record). Tests may capture it.
  std::function<void(std::string payload)> beacon_hook;

  // Test/chaos seams: pick the next probe target and see what is pending.
  std::optional<std::string> pending_ping_target() const;

 private:
  struct InFlightProbe {
    std::string target;
    std::int64_t ack_deadline_ms;      // direct ack expected by then
    std::int64_t indirect_deadline_ms;  // indirect results expected by then
    bool indirected{false};
    int indirect_results{0};
    std::int64_t seq;
  };

  struct IndirectForward {
    std::string originator;
    std::string target;
    std::int64_t originator_seq;  // echoed so stale acks never match
    std::int64_t deadline_ms;
  };

  void start_ping();
  void send_ping(const std::string& to, std::int64_t seq);
  void expire_forwardings();
  void expire_and_probe();
  void maybe_beacon();
  void flush_gossip(nlohmann::json& message);
  void enqueue_update(const Member& member, bool force);
  std::vector<Member> pick_indirect_probers(const std::string& target);
  std::optional<std::string> pick_probe_target();
  void mark_suspect(const std::string& device_id);
  void mark_dead(const std::string& device_id);
  void on_gossip_updates(const nlohmann::json& updates);
  std::int64_t next_seq();

  Options options_;
  Membership membership_;
  std::unique_ptr<Clock> default_clock_;
  Clock* clock_{default_clock_.get()};
  Transport* transport_{nullptr};

  std::mt19937 rng_;
  std::deque<Gossip> gossip_queue_;
  std::map<std::string, InFlightProbe> in_flight_;  // by target
  std::map<std::int64_t, IndirectForward> indirect_forwards_;  // by seq
  std::map<std::string, std::int64_t> suspect_deadlines_;  // device -> ms
  std::int64_t next_ping_ms_{0};
  std::int64_t next_beacon_ms_{0};
  std::int64_t seq_{1};
  bool leaving_{false};
};

}  // namespace onion::swim
