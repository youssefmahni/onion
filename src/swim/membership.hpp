#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace onion::swim {

// DESIGN › Membership: the eight membership states and their ownership rules.
enum class MemberState : int {
  INVITED = 0,  // holds a valid invitation, not yet admitted
  JOINING,      // admitted; cloning and catching up
  ACTIVE,       // synced and counted in ownership
  SUSPECT,      // missed probes; may still be alive; owns until DEAD
  LEAVING,      // no new work; draining in-flight nodes
  LEFT,         // gone; its log stays in every replica
  DEAD,         // gone; failed to refute suspicion
  EVICTED,      // key revoked or retired; terminal
};

std::string_view to_string(MemberState state);
std::optional<MemberState> parse_state(std::string_view value);

// Only ACTIVE (and SUSPECT, until it becomes DEAD) counts in ownership.
bool owns_work(MemberState state);

struct Member {
  std::string device_id;
  std::string address;          // host:port the device is reachable on
  MemberState state{MemberState::INVITED};
  std::int64_t incarnation{0};  // refutation is incarnation + 1
  std::map<std::string, std::int64_t> vector;  // origin -> last_seq
  nlohmann::json capabilities = nlohmann::json::object();

  nlohmann::json to_json() const;
  static std::optional<Member> from_json(const nlohmann::json& value,
                                         std::string* error);

  // The order states may be overwritten in: gossip merges never regress a
  // member along this order except by the explicit rules below.
  static int rank(MemberState state);
};

// Dissemination merge rule (DESIGN › Membership › Dissemination): the
// update with the higher incarnation wins; ALIVE with incarnation + 1
// refutes SUSPECT. LEFT/DEAD members come back through JOINING; EVICTED is
// terminal and nothing except another EVICTED record may replace it.
// Returns true when dst changed.
bool merge(Member& dst, const Member& incoming);

// A table of every known member plus the local device's own state.
class Membership {
 public:
  explicit Membership(std::string self_id);

  void upsert(const Member& member);
  const Member* get(std::string_view device_id) const;
  std::vector<Member> list() const;
  std::vector<Member> others() const;  // list() minus self
  std::size_t size() const { return members_.size(); }
  bool empty_of_others() const { return others().empty(); }

  void set_self_state(MemberState state, std::int64_t incarnation);
  Member self() const;

 private:
  std::string self_id_;
  std::map<std::string, Member> members_;
};

}  // namespace onion::swim
