#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace onion::reactconv {

// Decision 0027, PR3: a converter from RE&CT YAML action files
// (github.com/atc-project/atc-react) to the dev-import CSV shape.
// RA codes become node codes, and the whole import lands in the
// "react" tree.

struct Action {
  std::string id;     // "RA1001"
  std::string title;  // "Practice"
  std::string stage;  // "preparation"
};

// Parses one RE&CT action document. Only top-level scalar fields are
// read (id:, title:, stage:); block scalars (workflow:, automation:)
// are ignored, so a line-oriented scan is enough.
std::optional<Action> parse_action_yaml(std::string_view text);

// The canonical stage order; unknown stages sort after them.
const std::vector<std::string>& stage_order();

// Stage code for a stage name ("preparation" -> "PREPARATION").
std::string stage_code(std::string_view stage);

// Conversion result: stage roots (layer 0) and actions (layer 1,
// path = <STAGE>-<RA code>).
struct Converted {
  std::vector<std::pair<std::string, std::string>> stages;  // code, title
  std::vector<std::tuple<std::string, std::string, std::string>>
      actions;  // code, path, title
};

Converted convert(const std::vector<Action>& actions);

}  // namespace onion::reactconv
