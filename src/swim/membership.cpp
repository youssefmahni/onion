#include "swim/membership.hpp"

namespace onion::swim {

std::string_view to_string(MemberState state) {
  switch (state) {
    case MemberState::INVITED: return "INVITED";
    case MemberState::JOINING: return "JOINING";
    case MemberState::ACTIVE: return "ACTIVE";
    case MemberState::SUSPECT: return "SUSPECT";
    case MemberState::LEAVING: return "LEAVING";
    case MemberState::LEFT: return "LEFT";
    case MemberState::DEAD: return "DEAD";
    case MemberState::EVICTED: return "EVICTED";
  }
  return "INVITED";
}

std::optional<MemberState> parse_state(std::string_view value) {
  if (value == "INVITED") return MemberState::INVITED;
  if (value == "JOINING") return MemberState::JOINING;
  if (value == "ACTIVE") return MemberState::ACTIVE;
  if (value == "SUSPECT") return MemberState::SUSPECT;
  if (value == "LEAVING") return MemberState::LEAVING;
  if (value == "LEFT") return MemberState::LEFT;
  if (value == "DEAD") return MemberState::DEAD;
  if (value == "EVICTED") return MemberState::EVICTED;
  return std::nullopt;
}

bool owns_work(MemberState state) {
  return state == MemberState::ACTIVE || state == MemberState::SUSPECT;
}

nlohmann::json Member::to_json() const {
  nlohmann::json vector_json = nlohmann::json::object();
  for (const auto& [origin, seq] : vector)
    vector_json[origin] = seq;
  return {
      {"device_id", device_id},
      {"address", address},
      {"state", std::string(to_string(state))},
      {"incarnation", incarnation},
      {"vector", std::move(vector_json)},
      {"capabilities", capabilities},
  };
}

std::optional<Member> Member::from_json(const nlohmann::json& value,
                                        std::string* error) {
  if (!value.is_object() || !value.contains("device_id") ||
      !value["device_id"].is_string()) {
    if (error) *error = "member: missing device_id";
    return std::nullopt;
  }
  Member member;
  member.device_id = value["device_id"].get<std::string>();
  if (value.contains("address") && value["address"].is_string())
    member.address = value["address"].get<std::string>();
  if (value.contains("state")) {
    if (!value["state"].is_string()) {
      if (error) *error = "member: state must be a string";
      return std::nullopt;
    }
    const auto state =
        parse_state(value["state"].get<std::string>());
    if (!state) {
      if (error) *error = "member: unknown state";
      return std::nullopt;
    }
    member.state = *state;
  }
  if (value.contains("incarnation") && value["incarnation"].is_number_integer())
    member.incarnation = value["incarnation"].get<std::int64_t>();
  if (value.contains("vector") && value["vector"].is_object()) {
    for (auto it = value["vector"].begin(); it != value["vector"].end(); ++it)
      if (it.value().is_number_integer())
        member.vector[it.key()] = it.value().get<std::int64_t>();
  }
  if (value.contains("capabilities") && value["capabilities"].is_object())
    member.capabilities = value["capabilities"];
  return member;
}

int Member::rank(MemberState state) {
  switch (state) {
    case MemberState::INVITED: return 0;
    case MemberState::JOINING: return 1;
    case MemberState::ACTIVE: return 2;
    case MemberState::SUSPECT: return 3;
    case MemberState::LEAVING: return 4;
    case MemberState::LEFT: return 5;
    case MemberState::DEAD: return 6;
    case MemberState::EVICTED: return 7;
  }
  return 0;
}

bool merge(Member& dst, const Member& incoming) {
  if (incoming.device_id != dst.device_id) return false;
  if (dst.state == MemberState::EVICTED &&
      incoming.state != MemberState::EVICTED)
    return false;  // EVICTED is terminal

  bool changed = false;

  if (dst.state == MemberState::SUSPECT &&
      incoming.state == MemberState::ACTIVE &&
      incoming.incarnation > dst.incarnation) {
    // Refutation: ALIVE with incarnation + 1 restores ACTIVE.
    dst.state = MemberState::ACTIVE;
    changed = true;
  } else if ((dst.state == MemberState::LEFT ||
              dst.state == MemberState::DEAD) &&
             incoming.state == MemberState::JOINING &&
             incoming.incarnation >= dst.incarnation) {
    // DESIGN: LEFT and DEAD devices come back through JOINING.
    dst.state = MemberState::JOINING;
    changed = true;
  } else if (incoming.incarnation > dst.incarnation) {
    // A higher incarnation wins, except that LEFT/DEAD members are only
    // revived through JOINING, never directly into ACTIVE.
    const bool direct_revival = (dst.state == MemberState::LEFT ||
                                 dst.state == MemberState::DEAD) &&
                                incoming.state == MemberState::ACTIVE;
    if (!direct_revival) {
      dst.state = incoming.state;
      changed = true;
    }
  } else if (incoming.incarnation == dst.incarnation &&
             Member::rank(incoming.state) > Member::rank(dst.state)) {
    dst.state = incoming.state;
    changed = true;
  }

  if (incoming.incarnation > dst.incarnation) {
    dst.incarnation = incoming.incarnation;
    changed = true;
  }
  if (!incoming.address.empty() && incoming.address != dst.address) {
    dst.address = incoming.address;
    changed = true;
  }
  for (const auto& [origin, seq] : incoming.vector) {
    auto it = dst.vector.find(origin);
    if (it == dst.vector.end() || seq > it->second) {
      dst.vector[origin] = seq;
      changed = true;
    }
  }
  if (incoming.capabilities.is_object() &&
      incoming.capabilities != dst.capabilities) {
    dst.capabilities = incoming.capabilities;
    changed = true;
  }
  return changed;
}

Membership::Membership(std::string self_id) : self_id_(std::move(self_id)) {
  Member self;
  self.device_id = self_id_;
  self.state = MemberState::INVITED;
  members_[self_id_] = std::move(self);
}

void Membership::upsert(const Member& member) {
  auto it = members_.find(member.device_id);
  if (it == members_.end()) {
    members_[member.device_id] = member;
    return;
  }
  merge(it->second, member);
}

const Member* Membership::get(std::string_view device_id) const {
  const auto it = members_.find(std::string(device_id));
  return it == members_.end() ? nullptr : &it->second;
}

std::vector<Member> Membership::list() const {
  std::vector<Member> result;
  result.reserve(members_.size());
  for (const auto& [id, member] : members_) result.push_back(member);
  return result;
}

std::vector<Member> Membership::others() const {
  std::vector<Member> result;
  for (const auto& [id, member] : members_)
    if (id != self_id_) result.push_back(member);
  return result;
}

void Membership::set_self_state(MemberState state, std::int64_t incarnation) {
  auto& self = members_[self_id_];
  self.state = state;
  self.incarnation = incarnation;
}

Member Membership::self() const { return members_.at(self_id_); }

}  // namespace onion::swim
