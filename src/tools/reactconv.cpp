#include "tools/reactconv.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace onion::reactconv {

namespace {
std::string trim(std::string_view text) {
  std::size_t a = text.find_first_not_of(" \t\r\n");
  std::size_t b = text.find_last_not_of(" \t\r\n");
  if (a == std::string_view::npos) return {};
  return std::string(text.substr(a, b - a + 1));
}
}  // namespace

std::optional<Action> parse_action_yaml(std::string_view text) {
  Action action;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) continue;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    const std::string key = trim(std::string_view(line).substr(0, colon));
    const std::string value =
        trim(std::string_view(line).substr(colon + 1));
    if (key == "id") action.id = value;
    else if (key == "title") action.title = value;
    else if (key == "stage") action.stage = value;
  }
  if (action.id.empty() || action.title.empty() || action.stage.empty())
    return std::nullopt;
  return action;
}

const std::vector<std::string>& stage_order() {
  static const std::vector<std::string> kOrder = {
      "preparation", "identification", "containment", "eradication",
      "recovery",    "lessons_learned"};
  return kOrder;
}

std::string stage_code(std::string_view stage) {
  std::string code;
  for (const unsigned char c : stage)
    code.push_back(static_cast<char>(std::toupper(c)));
  return code;
}

Converted convert(const std::vector<Action>& actions) {
  Converted out;
  // Index of each stage in the canonical order (unknown = size()).
  std::unordered_map<std::string, int> rank;
  for (std::size_t i = 0; i < stage_order().size(); ++i)
    rank[stage_order()[i]] = static_cast<int>(i);
  // Stage roots, in canonical order, then any unknown stages.
  std::vector<std::string> stages_seen;
  for (const Action& action : actions)
    if (std::find(stages_seen.begin(), stages_seen.end(), action.stage) ==
        stages_seen.end())
      stages_seen.push_back(action.stage);
  std::stable_sort(stages_seen.begin(), stages_seen.end(),
                   [&](const std::string& a, const std::string& b) {
                     const int ra = rank.count(a) ? rank[a]
                                                  : static_cast<int>(stage_order().size());
                     const int rb = rank.count(b) ? rank[b]
                                                  : static_cast<int>(stage_order().size());
                     if (ra != rb) return ra < rb;
                     return a < b;
                   });
  for (const std::string& stage : stages_seen)
    out.stages.emplace_back(stage_code(stage), stage);
  // Actions, grouped by stage, then by id.
  std::vector<Action> sorted = actions;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const Action& a, const Action& b) {
                     return a.id < b.id;
                   });
  for (const std::string& stage : stages_seen)
    for (const Action& action : sorted)
      if (action.stage == stage)
        out.actions.emplace_back(action.id,
                                 stage_code(stage) + "-" + action.id,
                                 action.title);
  return out;
}

}  // namespace onion::reactconv
