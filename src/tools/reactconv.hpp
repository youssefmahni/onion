#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace onion::reactconv {

// Decision 0027, PR3: a converter from RE&CT YAML action files
// (github.com/atc-project/atc-react) to the dev-import CSV shape.
// The first and second digits of a response-action ID identify its stage and
// category.  The converter materializes both imported parent layers, and the
// whole import lands in the "react" tree.

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

// Conversion result: stage roots (layer 0), categories (layer 1), and
// response actions (layer 2, path = S<stage>-C<category>-<RA code>).
struct Converted {
  std::vector<std::pair<std::string, std::string>> stages;  // code, title
  std::vector<std::tuple<std::string, std::string, std::string>>
      categories;  // code, path, title
  std::vector<std::tuple<std::string, std::string, std::string>>
      actions;  // code, path, title
};

// Human action names from the RE&CT STIX bundle (docs/react.json in the
// atc-react repository): response-action id ("RA3101") -> name ("Block
// external IP address"). Empty when the text is not a bundle.
std::map<std::string, std::string> parse_action_names(
    std::string_view bundle_json);

// A readable title from a YAML slug title: "RA_3101_block_external_ip_address"
// -> "Block external ip address". A title that is not a slug is returned
// unchanged. The bundle name is preferred because a slug loses acronym case.
std::string humanize_title(std::string_view title);

// `names` (optional) overrides each action's YAML title with its human name.
Converted convert(const std::vector<Action>& actions,
                  const std::map<std::string, std::string>& names = {});

}  // namespace onion::reactconv
