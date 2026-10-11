#include "swim/swim.hpp"

#include <algorithm>
#include <chrono>

namespace onion::swim {

namespace {

constexpr std::string_view kMsgType = "t";
constexpr std::string_view kPing = "ping";
constexpr std::string_view kAck = "ack";
constexpr std::string_view kIndirectPing = "indirect_ping";
constexpr std::string_view kIndirectAck = "indirect_ack";
constexpr std::string_view kGossip = "gossip";
constexpr std::string_view kFrom = "from";
constexpr std::string_view kSeq = "seq";
constexpr std::string_view kTarget = "target";
constexpr std::string_view kOk = "ok";
constexpr std::string_view kUpdates = "updates";

std::string json_string(const nlohmann::json& value, std::string_view key,
                        std::string fallback = "") {
  if (!value.contains(key) || !value[key].is_string()) return fallback;
  return value[key].get<std::string>();
}

std::int64_t json_int(const nlohmann::json& value, std::string_view key,
                      std::int64_t fallback = 0) {
  if (!value.contains(key) || !value[key].is_number_integer()) return fallback;
  return value[key].get<std::int64_t>();
}

}  // namespace

std::int64_t SteadyClock::now_ms() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Swim::Swim(Options options)
    : options_(std::move(options)),
      membership_(options_.self_id),
      default_clock_(std::make_unique<SteadyClock>()),
      rng_(std::random_device{}()) {
  Member self = mutable_membership().self();
  self.address = options_.self_address;
  mutable_membership().upsert(self);
  next_ping_ms_ = clock_->now_ms();
  next_beacon_ms_ = clock_->now_ms() + options_.beacon_period_ms;
}

void Swim::set_transport(Transport* transport) { transport_ = transport; }

void Swim::set_clock(Clock* clock) {
  clock_ = clock;
  next_ping_ms_ = clock_->now_ms();
  next_beacon_ms_ = clock_->now_ms() + options_.beacon_period_ms;
}

void Swim::tick() {
  const std::int64_t now = clock_->now_ms();
  advance_timers();
  if (now >= next_ping_ms_ && in_flight_.empty()) start_ping();
  maybe_beacon();
}

// Test/chaos seam: the failure-detection timers only (suspicion expiry,
// forwarded-ping expiry, probe deadlines) without starting new probes or
// the beacon.
void Swim::advance_timers() {
  expire_forwardings();
  expire_and_probe();
}

void Swim::probe_now(const std::string& device_id) {
  if (!membership_.get(device_id)) return;
  if (in_flight_.count(device_id)) return;
  const std::int64_t now = clock_->now_ms();
  const std::int64_t seq = next_seq();
  InFlightProbe probe;
  probe.target = device_id;
  probe.seq = seq;
  probe.ack_deadline_ms = now + options_.ack_timeout_ms;
  probe.indirect_deadline_ms =
      now + options_.ack_timeout_ms + options_.indirect_timeout_ms;
  in_flight_[device_id] = probe;
  send_ping(device_id, seq);
}

std::optional<std::string> Swim::pending_ping_target() const {
  if (in_flight_.empty()) return std::nullopt;
  return in_flight_.begin()->second.target;
}

void Swim::start_ping() {
  const auto target = pick_probe_target();
  if (!target) {
    next_ping_ms_ = clock_->now_ms() + options_.period_ms;
    return;
  }
  const std::int64_t now = clock_->now_ms();
  const std::int64_t seq = next_seq();
  InFlightProbe probe;
  probe.target = *target;
  probe.seq = seq;
  probe.ack_deadline_ms = now + options_.ack_timeout_ms;
  probe.indirect_deadline_ms =
      now + options_.ack_timeout_ms + options_.indirect_timeout_ms;
  // Register before sending: a synchronous transport answers the ping
  // before this call returns.
  in_flight_[*target] = probe;
  send_ping(*target, seq);
}

void Swim::send_ping(const std::string& to, std::int64_t seq) {
  if (!transport_) return;
  nlohmann::json message = {
      {std::string(kMsgType), std::string(kPing)},
      {std::string(kFrom), options_.self_id},
      {std::string(kSeq), seq},
  };
  flush_gossip(message);
  const Member* target = membership_.get(to);
  transport_->send(target ? target->address : to, message.dump());
}

// An indirect ping we forward on behalf of another member: remember who
// asked so the target's ack (or the timeout) is reported back to them.
struct IndirectForward {
  std::string originator;
  std::string target;
  std::int64_t deadline_ms;
};

void Swim::expire_forwardings() {
  const std::int64_t now = clock_->now_ms();
  std::vector<std::int64_t> timed_out;
  for (const auto& [seq, forward] : indirect_forwards_)
    if (now >= forward.deadline_ms) timed_out.push_back(seq);
  for (const std::int64_t local_seq : timed_out) {
    const IndirectForward forward = indirect_forwards_[local_seq];
    indirect_forwards_.erase(local_seq);
    if (!transport_) continue;
    nlohmann::json report = {
        {std::string(kMsgType), std::string(kIndirectAck)},
        {std::string(kFrom), options_.self_id},
        {std::string(kSeq), forward.originator_seq},
        {std::string(kTarget), forward.target},
        {std::string(kOk), false},
    };
    const Member* originator = membership_.get(forward.originator_id);
    transport_->send(originator && !originator->address.empty()
                         ? originator->address
                         : forward.originator_address,
                     report.dump());
  }
}

void Swim::expire_and_probe() {
  const std::int64_t now = clock_->now_ms();

  // Suspicion expiry: no refutation within the suspicion timeout → DEAD.
  std::vector<std::string> dead_now;
  for (const auto& [device_id, deadline] : suspect_deadlines_)
    if (now >= deadline) dead_now.push_back(device_id);
  for (const auto& device_id : dead_now) mark_dead(device_id);

  // In-flight probes: direct ack missing → ask indirect probers; indirect
  // results missing → SUSPECT. Sends are deferred past the loop: a
  // synchronous transport answers before send() returns, and the answer
  // may erase from in_flight_.
  std::vector<std::pair<std::string, std::string>> sends;
  std::vector<std::string> suspect_now;
  for (auto& [target, probe] : in_flight_) {
    if (now >= probe.ack_deadline_ms && !probe.indirected) {
      probe.indirected = true;
      const auto probers = pick_indirect_probers(target);
      for (const auto& prober : probers) {
        nlohmann::json message = {
            {std::string(kMsgType), std::string(kIndirectPing)},
            {std::string(kFrom), options_.self_id},
            {std::string(kSeq), probe.seq},
            {std::string(kTarget), target},
        };
        flush_gossip(message);
        sends.emplace_back(prober.address, message.dump());
      }
      // The indirect phase gets its full window from when the indirect
      // pings went out, so a 1 s tick cadence cannot skip it.
      if (!probers.empty())
        probe.indirect_deadline_ms = now + options_.indirect_timeout_ms;
    }
    if (now >= probe.indirect_deadline_ms) suspect_now.push_back(target);
  }
  for (const auto& [address, bytes] : sends)
    if (transport_) transport_->send(address, bytes);
  // A positive indirect ack may have arrived during the sends above and
  // cleared the probe; only still-pending targets become SUSPECT.
  for (const auto& target : suspect_now) {
    if (!in_flight_.count(target)) continue;
    mark_suspect(target);
    in_flight_.erase(target);
  }
}

void Swim::maybe_beacon() {
  const std::int64_t now = clock_->now_ms();
  if (now < next_beacon_ms_) return;
  next_beacon_ms_ = now + options_.beacon_period_ms;
  if (!beacon_hook) return;
  nlohmann::json payload = {
      {std::string(kMsgType), "beacon"},
      {std::string(kFrom), options_.self_id},
      {std::string(kSeq), next_seq()},
  };
  flush_gossip(payload);
  beacon_hook(payload.dump());
}

void Swim::handle_datagram(const std::string& from_address,
                           std::string_view datagram) {
  nlohmann::json message;
  try {
    message = nlohmann::json::parse(datagram);
  } catch (const nlohmann::json::exception&) {
    return;  // undecipherable datagrams are dropped
  }
  if (!message.is_object()) return;

  const std::string type = json_string(message, kMsgType);
  const std::string from = json_string(message, kFrom);
  const std::int64_t seq = json_int(message, kSeq);

  const auto merge_updates = [&]() {
    if (message.contains(kUpdates) && message[kUpdates].is_array())
      on_gossip_updates(message[kUpdates]);
  };

  if (type == kGossip) {
    merge_updates();
    return;
  }

  if (type == kPing) {
    // Dissemination: updates piggyback on pings as well as acks, so merge
    // them before replying. Replies go to the sender's registered
    // host:port address (the datagram's source address has no port).
    // Accepted risk (decision 0013): pings/acks are unauthenticated, so a
    // spoofed "from" only redirects our reply — membership state changes
    // still require signed updates.
    merge_updates();
    nlohmann::json reply = {
        {std::string(kMsgType), std::string(kAck)},
        {std::string(kFrom), options_.self_id},
        {std::string(kSeq), seq},
    };
    flush_gossip(reply);
    if (transport_) {
      const Member* sender = membership_.get(from);
      transport_->send(sender && !sender->address.empty() ? sender->address
                                                          : from_address,
                       reply.dump());
    }
    return;
  }

  if (type == kIndirectPing) {
    // Forward a ping to the target; the target's ack comes back to us and
    // we report the outcome to the originator. The forward uses a local
    // sequence number (originators correlate by target, and every node's
    // seq counter starts at 1, so the originator's seq would collide).
    const std::string target = json_string(message, kTarget);
    merge_updates();
    const std::int64_t local_seq = next_seq();
    nlohmann::json forward = {
        {std::string(kMsgType), std::string(kPing)},
        {std::string(kFrom), options_.self_id},
        {std::string(kSeq), local_seq},
    };
    flush_gossip(forward);
    if (transport_) {
      indirect_forwards_[local_seq] = IndirectForward{
          from, from_address, target, seq,
          clock_->now_ms() + options_.ack_timeout_ms,
      };
      const Member* target_member = membership_.get(target);
      transport_->send(target_member ? target_member->address : target,
                       forward.dump());
    }
    return;
  }

  if (type == kAck) {
    auto forward_it = indirect_forwards_.find(seq);
    if (forward_it != indirect_forwards_.end()) {
      // This ack answers an indirect ping we forwarded: report success,
      // echoing the originator's seq so a stale report never clears a
      // newer probe.
      const IndirectForward forward = forward_it->second;
      indirect_forwards_.erase(forward_it);
      if (transport_) {
        nlohmann::json report = {
            {std::string(kMsgType), std::string(kIndirectAck)},
            {std::string(kFrom), options_.self_id},
            {std::string(kSeq), forward.originator_seq},
            {std::string(kTarget), forward.target},
            {std::string(kOk), true},
        };
        const Member* originator = membership_.get(forward.originator_id);
        transport_->send(originator && !originator->address.empty()
                             ? originator->address
                             : forward.originator_address,
                         report.dump());
      }
      merge_updates();
      return;
    }
    auto it = in_flight_.find(from);
    if (it != in_flight_.end()) {
      in_flight_.erase(it);
      next_ping_ms_ = clock_->now_ms() + options_.period_ms;
    }
    merge_updates();
    return;
  }

  if (type == kIndirectAck) {
    const std::string target = json_string(message, kTarget);
    const bool ok = message.contains(kOk) && message[kOk].is_boolean() &&
                    message[kOk].get<bool>();
    auto it = in_flight_.find(target);
    if (it != in_flight_.end() && it->second.indirected &&
        seq == it->second.seq) {  // a report for this exact probe round
      it->second.indirect_results += ok ? 1 : 0;
      // One confirmed indirect reachability clears the probe.
      if (ok) {
        in_flight_.erase(it);
        next_ping_ms_ = clock_->now_ms() + options_.period_ms;
      }
    }
    return;
  }

  if (type == "beacon") {
    merge_updates();
    return;
  }
}

void Swim::flush_gossip(nlohmann::json& message) {
  if (gossip_queue_.empty()) return;
  nlohmann::json updates = nlohmann::json::array();
  int budget = options_.gossip_budget;
  while (!gossip_queue_.empty()) {
    const Gossip pending = gossip_queue_.front();
    if (!pending.force && budget <= 0) break;
    gossip_queue_.pop_front();
    updates.push_back(pending.member.to_json());
    if (!pending.force) budget--;
  }
  message[std::string(kUpdates)] = std::move(updates);
}

void Swim::enqueue_update(const Member& member, bool force) {
  for (auto it = gossip_queue_.begin(); it != gossip_queue_.end(); ++it) {
    if (it->member.device_id == member.device_id) {
      it->member = member;
      it->force = it->force || force;
      return;
    }
  }
  gossip_queue_.push_back(Gossip{member, force});
}

void Swim::gossip(const Member& member, bool force) {
  enqueue_update(member, force);
}

void Swim::announce() {
  const Member self = membership_.self();
  enqueue_update(self, true);
}

void Swim::refute_suspicion(std::int64_t heard_incarnation) {
  const Member self = membership_.self();
  if (self.state == MemberState::LEAVING || self.state == MemberState::LEFT)
    return;
  if (self.incarnation > heard_incarnation) return;  // already refuted
  membership_.set_self_state(
      MemberState::ACTIVE,
      std::max(self.incarnation, heard_incarnation) + 1);
  announce();
}

void Swim::start_leaving() {
  leaving_ = true;
  membership_.set_self_state(MemberState::LEAVING,
                             membership_.self().incarnation);
  announce();
}

void Swim::confirm_left() {
  membership_.set_self_state(MemberState::LEFT,
                             membership_.self().incarnation + 1);
  announce();
}

std::vector<Member> Swim::pick_indirect_probers(const std::string& target) {
  std::vector<Member> candidates;
  for (const auto& member : membership_.others()) {
    if (member.device_id == target) continue;
    if (member.state == MemberState::ACTIVE ||
        member.state == MemberState::SUSPECT)
      candidates.push_back(member);
  }
  std::shuffle(candidates.begin(), candidates.end(), rng_);
  if (candidates.size() > static_cast<std::size_t>(options_.indirect_probes))
    candidates.resize(options_.indirect_probes);
  return candidates;
}

std::optional<std::string> Swim::pick_probe_target() {
  std::vector<Member> candidates;
  for (const auto& member : membership_.others()) {
    if (member.state == MemberState::ACTIVE ||
        member.state == MemberState::SUSPECT)
      candidates.push_back(member);
  }
  if (candidates.empty()) return std::nullopt;
  std::shuffle(candidates.begin(), candidates.end(), rng_);
  return candidates.front().device_id;
}

void Swim::mark_suspect(const std::string& device_id) {
  const Member* current = membership_.get(device_id);
  if (!current) return;
  if (current->state != MemberState::ACTIVE &&
      current->state != MemberState::SUSPECT)
    return;
  const bool was_suspect = current->state == MemberState::SUSPECT;
  Member suspect = *current;
  suspect.state = MemberState::SUSPECT;
  membership_.upsert(suspect);
  // The suspicion clock starts at the ACTIVE→SUSPECT transition; further
  // failed probes do not extend it, so a dead member reaches DEAD within
  // the timeout.
  if (!was_suspect || !suspect_deadlines_.count(device_id))
    suspect_deadlines_[device_id] =
        clock_->now_ms() + options_.suspicion_timeout_ms;
  enqueue_update(suspect, true);
}

void Swim::mark_dead(const std::string& device_id) {
  suspect_deadlines_.erase(device_id);
  const Member* current = membership_.get(device_id);
  if (!current || current->state != MemberState::SUSPECT) return;
  Member dead = *current;
  dead.state = MemberState::DEAD;
  membership_.upsert(dead);
  enqueue_update(dead, true);
}

void Swim::on_gossip_updates(const nlohmann::json& updates) {
  for (const auto& update : updates) {
    std::string error;
    const auto member = Member::from_json(update, &error);
    if (!member) continue;
    if (member->device_id == options_.self_id) {
      // Hearing ourselves gossiped as SUSPECT triggers the refutation:
      // ALIVE with incarnation + 1 (DESIGN step 4).
      if (member->state == MemberState::SUSPECT)
        refute_suspicion(member->incarnation);
      continue;
    }
    const Member* before = membership_.get(member->device_id);
    const bool was_suspect =
        before && before->state == MemberState::SUSPECT;
    membership_.upsert(*member);
    const Member* after = membership_.get(member->device_id);
    // A suspicion learned through gossip runs the same clock as a local
    // one: DEAD at the timeout without any refutation.
    if (after && after->state == MemberState::SUSPECT &&
        !suspect_deadlines_.count(member->device_id))
      suspect_deadlines_[member->device_id] =
          clock_->now_ms() + options_.suspicion_timeout_ms;
    if (was_suspect && after && after->state != MemberState::SUSPECT)
      suspect_deadlines_.erase(member->device_id);
    if (after && (!before || before->state != after->state ||
                  before->incarnation != after->incarnation)) {
      enqueue_update(*after, after->state == MemberState::SUSPECT ||
                                 after->state == MemberState::DEAD);
    }
  }
}

std::int64_t Swim::next_seq() { return seq_++; }

}  // namespace onion::swim
