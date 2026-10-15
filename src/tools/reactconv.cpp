#include "tools/reactconv.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace onion::reactconv {

namespace {
std::string trim(std::string_view text) {
  std::size_t a = text.find_first_not_of(" \t\r\n");
  std::size_t b = text.find_last_not_of(" \t\r\n");
  if (a == std::string_view::npos) return {};
  return std::string(text.substr(a, b - a + 1));
}

std::optional<std::pair<int, int>> action_coordinates(std::string_view id) {
  if (id.size() != 6 || id.substr(0, 2) != "RA") return std::nullopt;
  for (std::size_t i = 2; i < id.size(); ++i)
    if (id[i] < '0' || id[i] > '9') return std::nullopt;
  const int stage = id[2] - '0';
  const int category = id[3] - '0';
  if (stage < 1 || stage > 6 || category < 0 || category > 6)
    return std::nullopt;
  return std::make_pair(stage, category);
}

const std::vector<std::string>& stage_titles() {
  static const std::vector<std::string> kTitles = {
      "Preparation", "Identification", "Containment", "Eradication",
      "Recovery", "Lessons Learned"};
  return kTitles;
}

const std::vector<std::string>& category_titles() {
  static const std::vector<std::string> kTitles = {
      "General", "Network", "Email", "File", "Process", "Configuration",
      "Identity"};
  return kTitles;
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
  const auto coordinates = action_coordinates(action.id);
  if (!coordinates ||
      action.stage != stage_order()[coordinates->first - 1])
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

std::map<std::string, std::string> parse_action_names(
    std::string_view bundle_json) {
  std::map<std::string, std::string> names;
  const nlohmann::json bundle =
      nlohmann::json::parse(bundle_json.begin(), bundle_json.end(), nullptr,
                            false);
  if (!bundle.is_object() || !bundle.contains("objects") ||
      !bundle["objects"].is_array())
    return names;
  for (const auto& object : bundle["objects"]) {
    // is_string() first: value("type", "") throws on a non-string type.
    if (!object.is_object() || !object.contains("type") ||
        !object["type"].is_string() ||
        object["type"].get<std::string>() != "x-react-action" ||
        !object.contains("name") || !object["name"].is_string() ||
        !object.contains("external_references") ||
        !object["external_references"].is_array())
      continue;
    for (const auto& reference : object["external_references"]) {
      if (!reference.is_object() || !reference.contains("external_id") ||
          !reference["external_id"].is_string())
        continue;
      const std::string id = reference["external_id"].get<std::string>();
      const std::string name = trim(object["name"].get<std::string>());
      if (action_coordinates(id) && !name.empty()) names[id] = name;
      break;
    }
  }
  return names;
}

std::string humanize_title(std::string_view title) {
  // "RA_3101_block_external_ip_address": RA_, four digits, _, then words.
  std::string text(title);
  if (text.size() > 8 && text.compare(0, 3, "RA_") == 0 &&
      text[7] == '_' &&
      std::all_of(text.begin() + 3, text.begin() + 7, [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    text = text.substr(8);
    std::replace(text.begin(), text.end(), '_', ' ');
    if (!text.empty())
      text[0] = static_cast<char>(
          std::toupper(static_cast<unsigned char>(text[0])));
  }
  return text;
}

Converted convert(const std::vector<Action>& actions,
                  const std::map<std::string, std::string>& names) {
  Converted out;
  std::vector<Action> valid;
  valid.reserve(actions.size());
  for (const Action& action : actions)
    if (const auto coordinates = action_coordinates(action.id);
        coordinates && action.stage == stage_order()[coordinates->first - 1])
      valid.push_back(action);

  std::stable_sort(valid.begin(), valid.end(), [](const Action& a,
                                                   const Action& b) {
    const auto left = *action_coordinates(a.id);
    const auto right = *action_coordinates(b.id);
    return std::tie(left.first, left.second, a.id) <
           std::tie(right.first, right.second, b.id);
  });

  std::vector<std::pair<int, int>> categories_seen;
  std::vector<int> stages_seen;
  for (const Action& action : valid) {
    const auto [stage, category] = *action_coordinates(action.id);
    if (std::find(stages_seen.begin(), stages_seen.end(), stage) ==
        stages_seen.end())
      stages_seen.push_back(stage);
    const std::pair<int, int> coordinates{stage, category};
    if (std::find(categories_seen.begin(), categories_seen.end(), coordinates) ==
        categories_seen.end())
      categories_seen.push_back(coordinates);
  }
  for (const int stage : stages_seen)
    out.stages.emplace_back("S" + std::to_string(stage),
                            stage_titles()[stage - 1]);
  for (const auto& [stage, category] : categories_seen) {
    const std::string stage_code = "S" + std::to_string(stage);
    const std::string category_code = "C" + std::to_string(category);
    out.categories.emplace_back(category_code,
                                stage_code + "-" + category_code,
                                category_titles()[category]);
  }
  for (const Action& action : valid) {
    const auto [stage, category] = *action_coordinates(action.id);
    out.actions.emplace_back(
        action.id, "S" + std::to_string(stage) + "-C" +
                       std::to_string(category) + "-" + action.id,
        names.count(action.id) ? names.at(action.id)
                               : humanize_title(action.title));
  }
  return out;
}

}  // namespace onion::reactconv
