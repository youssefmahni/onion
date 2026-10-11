#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <string>

#include "swim/swim.hpp"

namespace onion::swim {
namespace {

// An in-memory bus: engines register an address; send() delivers the
// datagram straight to the receiver's handle_datagram, so the whole
// protocol runs synchronously and deterministically under a FakeClock.
// A drop rule models partitions: datagrams matching (from, to) vanish.
class MemBus {
 public:
  void add(const std::string& address, Swim* engine) {
    engines_[address] = engine;
  }

  void drop(const std::string& from, const std::string& to) {
    drops_.push_back({from, to});
  }

  void deliver(const std::string& from, const std::string& to,
               const std::string& datagram) {
    for (const auto& [drop_from, drop_to] : drops_)
      if (drop_from == from && drop_to == to) return;
    const auto it = engines_.find(to);
    if (it == engines_.end()) return;          // unregistered target
    if (it->second == nullptr) return;          // silenced node
    it->second->handle_datagram(from, datagram);
  }

 private:
  std::map<std::string, Swim*> engines_;
  std::vector<std::pair<std::string, std::string>> drops_;
};

class MemTransport final : public Transport {
 public:
  explicit MemTransport(MemBus* bus, std::string own_address)
      : bus_(bus), own_address_(std::move(own_address)) {}

  void send(const std::string& address, std::string_view datagram) override {
    sent_.push_back({address, std::string(datagram)});
    bus_->deliver(own_address_, address, std::string(datagram));
  }

  void set_receiver(std::function<void(const std::string&, std::string)>)
      override {}

  // How many datagrams this engine sent to `address`.
  int count_to(const std::string& address) const {
    int count = 0;
    for (const auto& [to, bytes] : sent_)
      if (to == address) count++;
    return count;
  }

  std::vector<std::pair<std::string, std::string>> sent_;

 private:
  MemBus* bus_;
  std::string own_address_;
};

Options node_options(const std::string& id, const std::string& address,
                     std::int64_t suspicion_timeout_ms = 60'000) {
  Options options;
  options.self_id = id;
  options.self_address = address;
  options.suspicion_timeout_ms = suspicion_timeout_ms;
  return options;
}

void set_active(Swim& engine, const std::string& id,
                const std::string& address, std::int64_t incarnation = 1) {
  engine.mutable_membership().set_self_state(MemberState::ACTIVE, incarnation);
  Member self = engine.membership().self();
  self.address = address;
  engine.mutable_membership().upsert(self);
}

void know(Swim& engine, const std::string& id, const std::string& address,
          MemberState state, std::int64_t incarnation = 1) {
  Member member;
  member.device_id = id;
  member.address = address;
  member.state = state;
  member.incarnation = incarnation;
  engine.mutable_membership().upsert(member);
}

// Four nodes, all ACTIVE and fully known to each other.
struct FourNodes {
  FakeClock clock{0};
  MemBus bus;
  Swim a{node_options("dev-a", "a:1")};
  Swim b{node_options("dev-b", "b:1")};
  Swim c{node_options("dev-c", "c:1")};
  Swim d{node_options("dev-d", "d:1")};
  MemTransport ta{&bus, "a:1"};
  MemTransport tb{&bus, "b:1"};
  MemTransport tc{&bus, "c:1"};
  MemTransport td{&bus, "d:1"};

  FourNodes() {
    for (auto* engine : {&a, &b, &c, &d}) engine->set_clock(&clock);
    a.set_transport(&ta);
    b.set_transport(&tb);
    c.set_transport(&tc);
    d.set_transport(&td);
    bus.add("a:1", &a);
    bus.add("b:1", &b);
    bus.add("c:1", &c);
    bus.add("d:1", &d);
    set_active(a, "dev-a", "a:1");
    set_active(b, "dev-b", "b:1");
    set_active(c, "dev-c", "c:1");
    set_active(d, "dev-d", "d:1");
    for (auto* engine : {&a, &b, &c, &d}) {
      know(*engine, "dev-a", "a:1", MemberState::ACTIVE);
      know(*engine, "dev-b", "b:1", MemberState::ACTIVE);
      know(*engine, "dev-c", "c:1", MemberState::ACTIVE);
      know(*engine, "dev-d", "d:1", MemberState::ACTIVE);
    }
  }

  // a probes dev-b deterministically: dev-c and dev-d leave a's probe pool.
  void isolate_target_on_a() {
    know(a, "dev-c", "c:1", MemberState::DEAD);
    know(a, "dev-d", "d:1", MemberState::DEAD);
  }

  // dev-b goes silent for everyone.
  void silence_b() { bus.add("b:1", nullptr); }

  // Whether `transport` sent an indirect_ack to a:1 since the last clear.
  static bool sent_indirect_ack(MemTransport& transport) {
    for (const auto& [to, bytes] : transport.sent_)
      if (to == "a:1" && bytes.find("\"indirect_ack\"") != std::string::npos)
        return true;
    return false;
  }

  // dev-b stops answering a only (one-way partition a → b).
  void partition_a_from_b() { bus.drop("a:1", "b:1"); }
};

TEST(SwimState, AllEightStatesParseAndRoundTrip) {
  for (const auto state :
       {MemberState::INVITED, MemberState::JOINING, MemberState::ACTIVE,
        MemberState::SUSPECT, MemberState::LEAVING, MemberState::LEFT,
        MemberState::DEAD, MemberState::EVICTED}) {
    const auto parsed = parse_state(to_string(state));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, state);
  }
  EXPECT_FALSE(parse_state("ONFIRE").has_value());
}

TEST(SwimState, OnlyActiveAndSuspectOwnWork) {
  EXPECT_TRUE(owns_work(MemberState::ACTIVE));
  EXPECT_TRUE(owns_work(MemberState::SUSPECT));
  for (const auto state :
       {MemberState::INVITED, MemberState::JOINING, MemberState::LEAVING,
        MemberState::LEFT, MemberState::DEAD, MemberState::EVICTED})
    EXPECT_FALSE(owns_work(state));
}

TEST(SwimState, EvictedIsTerminal) {
  Member member;
  member.device_id = "dev-x";
  member.state = MemberState::EVICTED;
  member.incarnation = 1;

  Member alive = member;
  alive.state = MemberState::ACTIVE;
  alive.incarnation = 2;

  Member dst = member;
  EXPECT_FALSE(merge(dst, alive));  // nothing replaces EVICTED
  EXPECT_EQ(dst.state, MemberState::EVICTED);

  Member another_eviction = member;
  another_eviction.incarnation = 9;
  EXPECT_TRUE(merge(dst, another_eviction));  // newer incarnation kept
  EXPECT_EQ(dst.incarnation, 9);
}

TEST(SwimState, AliveWithHigherIncarnationRefutesSuspect) {
  Member dst;
  dst.device_id = "dev-x";
  dst.state = MemberState::SUSPECT;
  dst.incarnation = 3;

  Member refuting = dst;
  refuting.state = MemberState::ACTIVE;
  refuting.incarnation = 4;  // incarnation + 1

  EXPECT_TRUE(merge(dst, refuting));
  EXPECT_EQ(dst.state, MemberState::ACTIVE);
  EXPECT_EQ(dst.incarnation, 4);
}

TEST(SwimState, StaleIncarnationIsIgnored) {
  Member dst;
  dst.device_id = "dev-x";
  dst.state = MemberState::ACTIVE;
  dst.incarnation = 5;

  Member stale = dst;
  stale.state = MemberState::SUSPECT;
  stale.incarnation = 2;

  EXPECT_FALSE(merge(dst, stale));
  EXPECT_EQ(dst.state, MemberState::ACTIVE);
  EXPECT_EQ(dst.incarnation, 5);
}

TEST(SwimState, VectorAndCapabilitiesMerge) {
  Member dst;
  dst.device_id = "dev-x";
  dst.state = MemberState::ACTIVE;
  dst.incarnation = 1;
  dst.vector["o1"] = 10;

  Member incoming = dst;
  incoming.vector["o1"] = 11;
  incoming.vector["o2"] = 7;
  incoming.capabilities = {{"ram_gb", 16}};

  EXPECT_TRUE(merge(dst, incoming));
  EXPECT_EQ(dst.vector.at("o1"), 11);
  EXPECT_EQ(dst.vector.at("o2"), 7);
  EXPECT_EQ(dst.capabilities.at("ram_gb"), 16);
}

TEST(SwimState, DeadComesBackThroughJoiningNotAlive) {
  Member dst;
  dst.device_id = "dev-x";
  dst.state = MemberState::DEAD;
  dst.incarnation = 3;

  Member direct_alive = dst;
  direct_alive.state = MemberState::ACTIVE;
  direct_alive.incarnation = 4;
  EXPECT_TRUE(merge(dst, direct_alive));   // incarnation still advances
  EXPECT_EQ(dst.state, MemberState::DEAD);  // but not straight to ACTIVE

  Member rejoining = dst;
  rejoining.state = MemberState::JOINING;
  rejoining.incarnation = 4;
  EXPECT_TRUE(merge(dst, rejoining));
  EXPECT_EQ(dst.state, MemberState::JOINING);

  Member active = rejoining;
  active.state = MemberState::ACTIVE;
  active.incarnation = 5;
  EXPECT_TRUE(merge(dst, active));
  EXPECT_EQ(dst.state, MemberState::ACTIVE);
}

TEST(SwimProtocol, AckKeepsMembersActive) {
  FourNodes nodes;
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");
  // The synchronous bus delivered b's ack before probe_now() returned:
  // the probe is already cleared and b stayed ACTIVE.
  EXPECT_FALSE(nodes.a.pending_ping_target().has_value());
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);
}

TEST(SwimProtocol, MissedPingWithNoProbersSuspects) {
  FourNodes nodes;
  nodes.silence_b();
  nodes.isolate_target_on_a();  // c and d are DEAD to a → no probers
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");

  // Direct ack deadline passes, then the indirect deadline: SUSPECT.
  nodes.clock.advance(1000);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);
}

TEST(SwimProtocol, TwoIndirectProbersForwardAndReport) {
  FourNodes nodes;
  nodes.silence_b();
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");

  nodes.clock.advance(500);  // ack deadline → a asks 2 indirect probers
  nodes.a.advance_timers();
  // Both c and d forwarded a ping to the silent b.
  EXPECT_EQ(nodes.tc.count_to("b:1"), 1);
  EXPECT_EQ(nodes.td.count_to("b:1"), 1);

  // Their forwards time out → both report failure to a. Only the timer
  // seam runs here, so c and d start no pings of their own.
  nodes.clock.advance(500);
  nodes.c.advance_timers();
  nodes.d.advance_timers();
  EXPECT_TRUE(FourNodes::sent_indirect_ack(nodes.tc));
  EXPECT_TRUE(FourNodes::sent_indirect_ack(nodes.td));
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);
}

TEST(SwimProtocol, IndirectProbeThroughAPartitionClearsTheProbe) {
  FourNodes nodes;
  // a cannot reach b directly, but c can: a one-way partition. Only d is
  // out of the pool, so c remains the one indirect prober.
  nodes.partition_a_from_b();
  know(nodes.a, "dev-d", "d:1", MemberState::DEAD);
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");

  nodes.clock.advance(500);
  nodes.a.advance_timers();  // a asks c; c's forward to b succeeds
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);

  // The indirect success cleared the probe: no suspicion later.
  nodes.clock.advance(1000);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);
}

TEST(SwimProtocol, RefutationRestoresActiveAndStopsTheClock) {
  FourNodes nodes;
  nodes.silence_b();
  nodes.isolate_target_on_a();
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");
  nodes.clock.advance(1000);
  nodes.a.advance_timers();
  ASSERT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);

  // dev-b is back. Only a remains in b's probe pool, so b's next probes
  // are guaranteed to reach a. a's ack piggybacks the SUSPECT update;
  // hearing itself suspected triggers b's refutation (ALIVE at +1).
  nodes.bus.add("b:1", &nodes.b);
  know(nodes.b, "dev-c", "c:1", MemberState::DEAD);
  know(nodes.b, "dev-d", "d:1", MemberState::DEAD);
  nodes.b.tick();  // b pings a → a acks with the suspicion → b refutes
  EXPECT_EQ(nodes.b.membership().self().state, MemberState::ACTIVE);
  EXPECT_EQ(nodes.b.membership().self().incarnation, 2);

  // b's next probe carries the refutation to a.
  nodes.clock.advance(1000);
  nodes.b.tick();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);
  EXPECT_EQ(nodes.a.membership().get("dev-b")->incarnation, 2);

  // Past the original suspicion deadline: still ACTIVE (the refutation
  // cleared the deadline).
  nodes.clock.advance(60'000);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);
}

TEST(SwimProtocol, SuspicionTimesOutToDead) {
  FourNodes nodes;
  nodes.silence_b();
  nodes.isolate_target_on_a();
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");
  nodes.clock.advance(1000);
  nodes.a.advance_timers();
  ASSERT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);

  nodes.clock.advance(60'000);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::DEAD);
}

TEST(SwimProtocol, PiggybackedGossipDisseminatesSuspicion) {
  FourNodes nodes;
  nodes.silence_b();
  nodes.isolate_target_on_a();
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");
  nodes.clock.advance(1000);
  nodes.a.advance_timers();
  ASSERT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);

  // dev-c pings a; a's ack piggybacks the SUSPECT update.
  nodes.bus.deliver("c:1", "a:1",
                    R"({"t":"ping","from":"dev-c","seq":41})");
  EXPECT_EQ(nodes.c.membership().get("dev-b")->state, MemberState::SUSPECT);
}

TEST(SwimProtocol, BeaconFiresEveryPeriodWithPayload) {
  FourNodes nodes;
  std::vector<std::string> payloads;
  nodes.a.beacon_hook = [&](std::string payload) {
    payloads.push_back(std::move(payload));
  };
  nodes.clock.set(0);
  nodes.a.tick();
  EXPECT_TRUE(payloads.empty());  // first beacon is one period out
  nodes.clock.advance(5000);
  nodes.a.tick();
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_NE(payloads.front().find("\"beacon\""), std::string::npos);
  nodes.clock.advance(5000);
  nodes.a.tick();
  ASSERT_EQ(payloads.size(), 2u);
}

TEST(SwimProtocol, LeaveAnnouncesLeavingThenLeft) {
  FourNodes nodes;
  nodes.clock.set(0);
  nodes.a.start_leaving();
  // b pings a: a's ack carries the LEAVING update.
  nodes.bus.deliver("b:1", "a:1",
                    R"({"t":"ping","from":"dev-b","seq":42})");
  EXPECT_EQ(nodes.b.membership().get("dev-a")->state, MemberState::LEAVING);

  nodes.a.confirm_left();
  nodes.bus.deliver("b:1", "a:1",
                    R"({"t":"ping","from":"dev-b","seq":43})");
  EXPECT_EQ(nodes.b.membership().get("dev-a")->state, MemberState::LEFT);
}

TEST(SwimProtocol, GossipLearnedSuspicionStillTimesOut) {
  FourNodes nodes;
  nodes.clock.set(0);
  // a learns dev-b is SUSPECT through gossip, not through its own probe.
  nodes.a.handle_datagram(
      "c:1",
      R"({"t":"gossip","from":"dev-c","updates":[{"device_id":"dev-b","address":"b:1","state":"SUSPECT","incarnation":1}]})");
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);

  // The suspicion clock runs from the moment the suspicion was learned;
  // no refutation → DEAD at the timeout even though a never probed b.
  nodes.clock.advance(60'000);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::DEAD);
}

TEST(SwimProtocol, StaleIndirectAckDoesNotClearAFreshProbe) {
  FourNodes nodes;
  nodes.silence_b();
  know(nodes.a, "dev-d", "d:1", MemberState::DEAD);  // only c probes
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");  // round 1: seq 1
  nodes.clock.advance(1000);
  nodes.a.advance_timers();  // indirect phase starts via c (b silent)
  nodes.clock.advance(500);
  nodes.c.advance_timers();  // c's forward times out → negative report
  nodes.a.advance_timers();  // the indirect deadline passes → SUSPECT
  ASSERT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);

  // A new probe round starts (seq 2); a stale positive report for round
  // 1's seq must not clear it.
  nodes.a.probe_now("dev-b");
  nodes.a.handle_datagram(
      "c:1",
      R"({"t":"indirect_ack","from":"dev-c","seq":1,"target":"dev-b","ok":true})");
  EXPECT_TRUE(nodes.a.pending_ping_target().has_value());
  nodes.clock.advance(1500);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);
}

TEST(SwimProtocol, RepeatedProbesDoNotExtendSuspicion) {
  FourNodes nodes;
  nodes.silence_b();
  nodes.isolate_target_on_a();
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");
  nodes.clock.advance(1000);
  nodes.a.advance_timers();
  ASSERT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);

  // Re-probe the suspected member every period: the suspicion clock must
  // not move, so DEAD still lands exactly at the timeout.
  for (std::int64_t t = 2000; t < 61'000; t += 1000) {
    nodes.clock.set(t);
    nodes.a.tick();
  }
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::SUSPECT);
  nodes.clock.set(61'000);
  nodes.a.tick();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::DEAD);
}

TEST(SwimProtocol, IndirectRescueInOneJumpKeepsTheTargetActive) {
  FourNodes nodes;
  nodes.partition_a_from_b();  // a cannot reach b, but c can
  know(nodes.a, "dev-d", "d:1", MemberState::DEAD);  // only c stays a prober
  nodes.clock.set(0);
  nodes.a.probe_now("dev-b");

  // One jump straight past the direct ack deadline: the indirect chain
  // (a → c → b → c → a) resolves synchronously and must rescue b before
  // the suspicion decision.
  nodes.clock.advance(1000);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);

  nodes.clock.advance(500);
  nodes.a.advance_timers();
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);
}

TEST(SwimProtocol, MalformedDatagramsAreDropped) {
  FourNodes nodes;
  nodes.clock.set(0);
  // Unparseable bytes, a type-error on a known field, and a gossip update
  // with a non-string state: all must be dropped, never thrown.
  nodes.a.handle_datagram("b:1", "{this is not json");
  nodes.a.handle_datagram("b:1",
                          R"({"t":"indirect_ack","ok":1,"target":"dev-b"})");
  nodes.a.handle_datagram(
      "b:1",
      R"({"t":"gossip","updates":[{"device_id":"dev-b","state":5}]})");
  EXPECT_EQ(nodes.a.membership().get("dev-b")->state, MemberState::ACTIVE);
  EXPECT_EQ(nodes.a.membership().self().state, MemberState::ACTIVE);
}

TEST(SwimProtocol, IdleWithNoPeersDoesNothing) {
  FakeClock clock(0);
  Options options = node_options("dev-x", "x:1");
  Swim solo(options);
  solo.set_clock(&clock);
  MemBus bus;
  MemTransport transport(&bus, "x:1");
  solo.set_transport(&transport);
  solo.mutable_membership().set_self_state(MemberState::ACTIVE, 1);

  for (int i = 0; i < 10; i++) {
    solo.tick();
    clock.advance(1000);
  }
  EXPECT_TRUE(transport.sent_.empty());
  EXPECT_EQ(solo.membership().self().state, MemberState::ACTIVE);
}

}  // namespace
}  // namespace onion::swim
