#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "log/log.hpp"

namespace onion::domain {

// Decisions 0027 and 0029: a profile owns deterministic title checks,
// per-level child contract, and (for non-planning domains) prompt text.
// The embedded planning profile deliberately has no v2 fields, so its
// historical canonical JSON and hash remain unchanged.

struct StyleChecks {
  // Original planning fields. Unset fields mean "no check".
  bool title_case{false};
  int max_words{0};
  bool no_questions{false};  // '?' and '!' banned (legacy behaviour)
  bool no_trailing_punct{false};  // trailing '.' or ':' banned (legacy)
  std::vector<std::string> filler_words;

  // Decision 0029 per-level title rules.
  int min_words{0};
  int max_chars{0};
  bool keep_product_capitalisation{false};
  bool starts_with_capital{false};
  bool starts_with_capital_or_product{false};
  bool no_emoji{false};
  bool no_trailing_period{false};
  // Tri-state: unset = no code-span check (the planning default); true =
  // a code span is allowed but its backticks must pair up; false = none.
  std::optional<bool> allow_code_span;
  std::string allowed_punctuation;
  // Semantic shape is prompt/critic guidance; it is not pretended to be an
  // unreliable deterministic grammar parser.
  std::string shape;
};

struct ChildRange {
  int min{};
  int max{};
};

struct LevelDefinition {
  int level{};
  std::string name;
  std::string source;
  std::string children_are;
  std::optional<ChildRange> children;
  bool ordered_children{false};
};

struct StyleLevel {
  std::vector<int> levels;
  StyleChecks style;
};

struct Profile {
  std::string name;
  std::string tree;  // "" = planning tree
  StyleChecks style;  // legacy/default style fallback
  std::vector<int> structure_layers;
  std::vector<int> imported_layers;
  std::optional<int> light_funnel_from;
  int max_level{-1};  // levels.max; -1 when the profile declares no levels
  std::vector<LevelDefinition> levels;
  std::vector<StyleLevel> style_levels;
  std::string description;
  std::string persona;
  std::string system_prompt;
  std::string atomic_definition;
  std::optional<bool> atomic_verify;
  std::optional<int> atomic_expected_from_level;
  // Complete validated source document. This makes every accepted profile
  // field part of the content address. Empty for the embedded planning
  // profile, whose old canonical form must remain byte-identical.
  nlohmann::json document;
};

inline const Profile& planning_profile() {
  static const Profile kPlanning = [] {
    Profile profile;
    profile.name = "planning";
    profile.tree = "";
    profile.style.title_case = true;
    profile.style.max_words = 8;
    profile.style.no_questions = true;
    profile.style.no_trailing_punct = true;
    profile.style.filler_words = {"the",  "a",     "an",   "of",
                                  "and",  "or",    "for",  "to",
                                  "in",   "on",    "at",   "with",
                                  "by",   "from",  "vs",   "versus",
                                  "into", "within"};
    profile.structure_layers = {0, 1, 2};
    return profile;
  }();
  return kPlanning;
}

// Characters, not bytes: a UTF-8 continuation byte (10xxxxxx) adds nothing.
inline int utf8_length(std::string_view text) {
  int length = 0;
  for (const unsigned char c : text)
    if ((c & 0xc0) != 0x80) ++length;
  return length;
}

inline bool ascii_punctuation(unsigned char c) {
  return c >= 33 && c <= 126 && !std::isalnum(c);
}

inline bool contains_emoji(std::string_view text) {
  // Decode only enough UTF-8 for emoji/symbol blocks; ordinary Unicode
  // product names stay valid.
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char first = static_cast<unsigned char>(text[i]);
    std::uint32_t codepoint = first;
    std::size_t width = 1;
    if ((first & 0xe0) == 0xc0 && i + 1 < text.size()) {
      codepoint = (first & 0x1f) << 6 |
                  (static_cast<unsigned char>(text[i + 1]) & 0x3f);
      width = 2;
    } else if ((first & 0xf0) == 0xe0 && i + 2 < text.size()) {
      codepoint = (first & 0x0f) << 12 |
                  (static_cast<unsigned char>(text[i + 1]) & 0x3f) << 6 |
                  (static_cast<unsigned char>(text[i + 2]) & 0x3f);
      width = 3;
    } else if ((first & 0xf8) == 0xf0 && i + 3 < text.size()) {
      codepoint = (first & 0x07) << 18 |
                  (static_cast<unsigned char>(text[i + 1]) & 0x3f) << 12 |
                  (static_cast<unsigned char>(text[i + 2]) & 0x3f) << 6 |
                  (static_cast<unsigned char>(text[i + 3]) & 0x3f);
      width = 4;
    }
    if ((codepoint >= 0x1f000 && codepoint <= 0x1faff) ||
        (codepoint >= 0x2600 && codepoint <= 0x27bf) ||
        codepoint == 0x200d || codepoint == 0xfe0f)
      return true;
    i += width;
  }
  return false;
}

inline bool starts_with_capital_or_product(std::string_view text) {
  if (text.empty()) return false;
  const unsigned char first = static_cast<unsigned char>(text.front());
  if (std::isupper(first) || std::isdigit(first)) return true;
  // Product names such as iOS and eBPF use an internal capital.
  for (std::size_t i = 1;
       i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]));
       ++i)
    if (std::isupper(static_cast<unsigned char>(text[i]))) return true;
  return false;
}

inline std::vector<std::string> style_flags(std::string_view title,
                                            const StyleChecks& checks) {
  std::vector<std::string> flags;
  const std::string text(title);
  // Preserve the legacy planning checks and their order exactly.
  if (checks.no_questions && (text.find('?') != std::string::npos ||
                              text.find('!') != std::string::npos))
    flags.push_back("question_or_bang");
  if (checks.title_case &&
      (text.empty() || !std::isupper(static_cast<unsigned char>(text[0]))))
    flags.push_back("not_title_case");
  std::size_t words = 0;
  {
    std::istringstream stream(text);
    std::string word;
    while (stream >> word) ++words;
  }
  if (checks.max_words > 0 && static_cast<int>(words) > checks.max_words)
    flags.push_back("too_many_words");
  if (checks.no_trailing_punct && !text.empty() &&
      (text.back() == '.' || text.back() == ':'))
    flags.push_back("trailing_punct");
  if (words > 2) {
    const std::unordered_set<std::string> fillers(checks.filler_words.begin(),
                                                  checks.filler_words.end());
    std::istringstream stream(text);
    std::string word;
    bool filler = false;
    while (stream >> word) {
      std::string lowered;
      for (const unsigned char c : word)
        lowered.push_back(static_cast<char>(std::tolower(c)));
      if (fillers.count(lowered)) filler = true;
    }
    if (filler) flags.push_back("contains_filler");
  }
  if (checks.min_words > 0 && static_cast<int>(words) < checks.min_words)
    flags.push_back("too_few_words");
  if (checks.max_chars > 0 && utf8_length(text) > checks.max_chars)
    flags.push_back("too_many_characters");
  if (checks.starts_with_capital &&
      (text.empty() || !std::isupper(static_cast<unsigned char>(text[0]))))
    flags.push_back("does_not_start_with_capital");
  if (checks.starts_with_capital_or_product &&
      !starts_with_capital_or_product(text))
    flags.push_back("does_not_start_with_capital_or_product");
  if (checks.no_emoji && contains_emoji(text)) flags.push_back("contains_emoji");
  if (checks.no_trailing_period && !text.empty() && text.back() == '.')
    flags.push_back("trailing_period");
  const std::size_t code_ticks =
      static_cast<std::size_t>(std::count(text.begin(), text.end(), '`'));
  // Only a profile that says so checks code spans: the planning style never
  // sets allow_code_span, and a backticked planning title stays valid.
  if (checks.allow_code_span.has_value()) {
    if (!*checks.allow_code_span && code_ticks != 0)
      flags.push_back("code_span");
    if (*checks.allow_code_span && code_ticks % 2 != 0)
      flags.push_back("unpaired_code_span");
  }
  if (!checks.allowed_punctuation.empty()) {
    for (const unsigned char c : text) {
      if (ascii_punctuation(c) &&
          checks.allowed_punctuation.find(static_cast<char>(c)) ==
              std::string::npos) {
        flags.push_back("disallowed_punctuation");
        break;
      }
    }
  }
  return flags;
}

inline const StyleChecks& style_for_child_level(const Profile& profile,
                                                int child_level) {
  for (const StyleLevel& level : profile.style_levels)
    if (std::find(level.levels.begin(), level.levels.end(), child_level) !=
        level.levels.end())
      return level.style;
  return profile.style;
}

// The definition that governs `level`: the exact one or, for a level deeper
// than the last definition but within levels.max, the last definition (a
// profile that ends its list at "Sub-operation" repeats it down to the cap).
// Beyond levels.max, or in a profile with no levels, there is none.
inline const LevelDefinition* definition_for_level(const Profile& profile,
                                                   int level) {
  if (profile.levels.empty() || level < 0) return nullptr;
  const LevelDefinition* last = nullptr;
  for (const LevelDefinition& definition : profile.levels) {
    if (definition.level == level) return &definition;
    if (last == nullptr || definition.level > last->level) last = &definition;
  }
  if (last != nullptr && level > last->level && level <= profile.max_level)
    return last;
  return nullptr;
}

inline std::optional<ChildRange> child_range_for_parent(
    const Profile& profile, int parent_level) {
  const LevelDefinition* definition = definition_for_level(profile, parent_level);
  return definition ? definition->children : std::nullopt;
}

inline bool ordered_children_for_parent(const Profile& profile,
                                        int parent_level) {
  const LevelDefinition* definition = definition_for_level(profile, parent_level);
  return definition && definition->ordered_children;
}

inline std::string level_name(const Profile& profile, int level) {
  const LevelDefinition* definition = definition_for_level(profile, level);
  return definition ? definition->name : "L" + std::to_string(level);
}

inline std::string child_level_guidance(const Profile& profile,
                                        int parent_level) {
  const LevelDefinition* definition = definition_for_level(profile, parent_level);
  return definition ? definition->children_are : std::string{};
}

inline std::string title_rules(const StyleChecks& checks) {
  std::vector<std::string> rules;
  if (!checks.shape.empty()) rules.push_back(checks.shape);
  if (checks.min_words > 0 || checks.max_words > 0) {
    if (checks.min_words > 0 && checks.max_words > 0)
      rules.push_back(std::to_string(checks.min_words) + "-" +
                      std::to_string(checks.max_words) + " words");
    else if (checks.min_words > 0)
      rules.push_back("at least " + std::to_string(checks.min_words) +
                      " words");
    else
      rules.push_back("at most " + std::to_string(checks.max_words) +
                      " words");
  }
  if (checks.max_chars > 0)
    rules.push_back("at most " + std::to_string(checks.max_chars) +
                    " characters");
  if (checks.title_case) rules.push_back("Title Case");
  if (checks.starts_with_capital_or_product)
    rules.push_back("starts with a capital or product capitalization");
  else if (checks.starts_with_capital)
    rules.push_back("starts with a capital");
  if (checks.no_questions) rules.push_back("no questions or exclamation marks");
  if (checks.no_emoji) rules.push_back("no emoji");
  if (checks.no_trailing_period) rules.push_back("no trailing period");
  if (checks.no_trailing_punct) rules.push_back("no trailing period or colon");
  if (!checks.filler_words.empty()) {
    std::string words;
    for (const std::string& word : checks.filler_words)
      words += (words.empty() ? "" : ", ") + word;
    rules.push_back("when a title has more than two words, none of them may be: " +
                    words);
  }
  if (!checks.allowed_punctuation.empty())
    rules.push_back("only punctuation: " + checks.allowed_punctuation);
  if (checks.allow_code_span.has_value())
    rules.push_back(*checks.allow_code_span
                        ? "one paired backtick code span allowed"
                        : "no code spans");
  if (rules.empty()) return "no additional title restrictions";
  std::ostringstream out;
  for (std::size_t i = 0; i < rules.size(); ++i)
    out << (i == 0 ? "" : "; ") << rules[i];
  return out.str();
}

namespace detail {
using json = nlohmann::json;

inline bool only_keys(const json& value,
                      std::initializer_list<std::string_view> allowed,
                      std::string_view context, std::string* error) {
  for (auto it = value.begin(); it != value.end(); ++it) {
    bool found = false;
    for (const std::string_view key : allowed)
      if (it.key() == key) found = true;
    if (!found) {
      if (error) *error = std::string(context) + " has unknown field " + it.key();
      return false;
    }
  }
  return true;
}

inline bool boolean_field(const json& object, const char* key, bool* target,
                          std::string_view context, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end()) return true;
  if (!it->is_boolean()) {
    if (error) *error = std::string(context) + "." + key + " must be a boolean";
    return false;
  }
  *target = it->get<bool>();
  return true;
}

inline bool parse_style(const json& value, StyleChecks* out,
                        std::string_view context, std::string* error,
                        bool style_level = false) {
  if (!value.is_object()) {
    if (error) *error = std::string(context) + " must be an object";
    return false;
  }
  if (!only_keys(value,
                 style_level
                     ? std::initializer_list<std::string_view>{
                           "levels", "shape", "min_words", "max_words", "max_chars",
                           "title_case", "keep_product_capitalisation",
                           "starts_with_capital", "starts_with_capital_or_product",
                           "no_questions", "no_emoji", "no_trailing_period",
                           "no_trailing_punct", "allow_code_span",
                           "allowed_punctuation", "filler_words"}
                     : std::initializer_list<std::string_view>{
                           "shape", "min_words", "max_words", "max_chars",
                           "title_case", "keep_product_capitalisation",
                           "starts_with_capital", "starts_with_capital_or_product",
                           "no_questions", "no_emoji", "no_trailing_period",
                           "no_trailing_punct", "allow_code_span",
                           "allowed_punctuation", "filler_words"},
                 context, error))
    return false;
  for (const auto entry : {std::pair<const char*, bool*>{"title_case", &out->title_case},
                           {"keep_product_capitalisation", &out->keep_product_capitalisation},
                           {"starts_with_capital", &out->starts_with_capital},
                           {"starts_with_capital_or_product", &out->starts_with_capital_or_product},
                           {"no_questions", &out->no_questions},
                           {"no_emoji", &out->no_emoji},
                           {"no_trailing_period", &out->no_trailing_period},
                           {"no_trailing_punct", &out->no_trailing_punct}})
    if (!boolean_field(value, entry.first, entry.second, context, error)) return false;
  if (const auto it = value.find("allow_code_span"); it != value.end()) {
    if (!it->is_boolean()) {
      if (error) *error = std::string(context) + ".allow_code_span must be a boolean";
      return false;
    }
    out->allow_code_span = it->get<bool>();
  }
  for (const auto entry : {std::pair<const char*, int*>{"min_words", &out->min_words},
                           {"max_words", &out->max_words},
                           {"max_chars", &out->max_chars}}) {
    const auto it = value.find(entry.first);
    if (it == value.end()) continue;
    if (!it->is_number_integer()) {
      if (error) *error = std::string(context) + "." + entry.first + " must be an integer";
      return false;
    }
    const std::int64_t number = it->get<std::int64_t>();
    if (number < 1 || number > 1000) {
      if (error) *error = std::string(context) + "." + entry.first +
                          " out of range 1..1000";
      return false;
    }
    *entry.second = static_cast<int>(number);
  }
  if (out->min_words > 0 && out->max_words > 0 &&
      out->min_words > out->max_words) {
    if (error) *error = std::string(context) + ".min_words exceeds max_words";
    return false;
  }
  if (out->starts_with_capital && out->starts_with_capital_or_product) {
    if (error) *error = std::string(context) +
                        " cannot require both capital-only and product capitalization";
    return false;
  }
  for (const auto entry : {std::pair<const char*, std::string*>{"shape", &out->shape},
                           {"allowed_punctuation", &out->allowed_punctuation}}) {
    const auto it = value.find(entry.first);
    if (it == value.end()) continue;
    if (!it->is_string()) {
      if (error) *error = std::string(context) + "." + entry.first + " must be a string";
      return false;
    }
    *entry.second = it->get<std::string>();
  }
  for (const unsigned char c : out->allowed_punctuation)
    if (!ascii_punctuation(c)) {
      if (error) *error = std::string(context) +
                          ".allowed_punctuation must contain ASCII punctuation only";
      return false;
    }
  if (value.contains("filler_words")) {
    if (!value["filler_words"].is_array()) {
      if (error) *error = std::string(context) + ".filler_words must be an array of strings";
      return false;
    }
    for (const auto& word : value["filler_words"]) {
      if (!word.is_string()) {
        if (error) *error = std::string(context) +
                            ".filler_words entries must be strings";
        return false;
      }
      out->filler_words.push_back(word.get<std::string>());
    }
  }
  return true;
}

inline json style_json(const StyleChecks& style) {
  json out = json::object();
  if (style.title_case) out["title_case"] = true;
  if (style.max_words > 0) out["max_words"] = style.max_words;
  if (style.no_questions) out["no_questions"] = true;
  if (style.no_trailing_punct) out["no_trailing_punct"] = true;
  if (!style.filler_words.empty()) out["filler_words"] = style.filler_words;
  if (style.min_words > 0) out["min_words"] = style.min_words;
  if (style.max_chars > 0) out["max_chars"] = style.max_chars;
  if (style.keep_product_capitalisation)
    out["keep_product_capitalisation"] = true;
  if (style.starts_with_capital) out["starts_with_capital"] = true;
  if (style.starts_with_capital_or_product)
    out["starts_with_capital_or_product"] = true;
  if (style.no_emoji) out["no_emoji"] = true;
  if (style.no_trailing_period) out["no_trailing_period"] = true;
  if (style.allow_code_span.has_value())
    out["allow_code_span"] = *style.allow_code_span;
  if (!style.allowed_punctuation.empty())
    out["allowed_punctuation"] = style.allowed_punctuation;
  if (!style.shape.empty()) out["shape"] = style.shape;
  return out;
}

inline bool parse_int_layers(const json& value, std::vector<int>* out,
                             std::string_view context, std::string* error) {
  if (!value.is_array()) {
    if (error) *error = std::string(context) + " must be an array of integers";
    return false;
  }
  std::unordered_set<int> seen;
  for (const auto& layer : value) {
    if (!layer.is_number_integer()) {
      if (error) *error = std::string(context) + " entries must be integers";
      return false;
    }
    const std::int64_t number = layer.get<std::int64_t>();
    if (number < 0 || number > 9) {
      if (error) *error = std::string(context) + " out of range 0..9";
      return false;
    }
    if (!seen.insert(static_cast<int>(number)).second) {
      if (error) *error = std::string(context) + " contains a duplicate layer";
      return false;
    }
    out->push_back(static_cast<int>(number));
  }
  return true;
}

inline bool validate_string_array(const json& value, std::string_view context,
                                  std::string* error) {
  if (!value.is_array()) {
    if (error) *error = std::string(context) + " must be an array of strings";
    return false;
  }
  for (const auto& item : value)
    if (!item.is_string()) {
      if (error) *error = std::string(context) + " entries must be strings";
      return false;
    }
  return true;
}

// The prompt stages and the placeholders each one can be given. The gate
// fills exactly these; a template that names anything else would reach the
// model with a literal "{name}" in it, so loading rejects it.
struct PromptContract {
  std::string_view name;
  std::vector<std::string_view> extra;     // allowed beyond the common set
  std::vector<std::string_view> required;  // must appear in the template
};

inline const std::vector<std::string_view>& common_placeholders() {
  static const std::vector<std::string_view> kCommon = {
      "title",        "path",         "level",         "level_name",
      "child_level_name", "child_level_guidance", "ancestors", "siblings",
      "existing_children", "children_min", "children_max", "title_rules",
      "atomic_definition", "critique_anchors"};
  return kCommon;
}

inline const std::vector<PromptContract>& prompt_contracts() {
  static const std::vector<PromptContract> kContracts = {
      {"precheck", {}, {"title"}},
      {"planner", {"n_aspects_min", "n_aspects_max", "retry_reasons"}, {"title"}},
      {"worker", {"aspect", "n_items"}, {"title", "aspect"}},
      {"trimmer", {"candidates", "keep"}, {"title", "candidates", "keep"}},
      {"critique", {"candidates"}, {"title", "candidates"}},
      {"synthesizer", {"candidates", "critique_issues", "retry_reasons"},
       {"title", "candidates"}},
      {"atomic_verification", {"labelled"}, {"title", "labelled"}},
  };
  return kContracts;
}

inline bool placeholder_known(std::string_view name) {
  for (const std::string_view common : common_placeholders())
    if (common == name) return true;
  for (const PromptContract& contract : prompt_contracts())
    for (const std::string_view extra : contract.extra)
      if (extra == name) return true;
  return false;
}

// Every placeholder-looking `{...}` marker in `text`: a body made only of
// letters, digits, underscore, hyphen and spaces, with at least one letter or
// digit. {Candidates}, {TITLE}, { title } and {made-up} all count, so they are
// refused unless they are exactly a known lowercase name; a JSON example such
// as {"splits": true} is not a marker (quotes and colons are not name
// characters).
inline std::vector<std::string> placeholders_in(std::string_view text) {
  std::vector<std::string> found;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '{') continue;
    const std::size_t close = text.find('}', i + 1);
    if (close == std::string_view::npos) break;
    const std::string_view body = text.substr(i + 1, close - i - 1);
    const bool name_characters =
        std::all_of(body.begin(), body.end(), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                 c == '-' || c == ' ';
        });
    const bool has_alnum =
        std::any_of(body.begin(), body.end(), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) != 0;
        });
    if (name_characters && has_alnum) found.emplace_back(body);
  }
  return found;
}

inline constexpr std::size_t kMaxProfileBytes = 256 * 1024;
inline constexpr std::size_t kMaxPromptBytes = 16 * 1024;

// PROFILE is a signed record, so its score labels use exact decimal text
// instead of JSON floating-point values (decision 0003). Accept 0..1 with
// any non-empty decimal precision, including 1.000.
inline bool unit_decimal(std::string_view value) {
  if (value.empty() || (value[0] != '0' && value[0] != '1')) return false;
  std::size_t index = 1;
  if (index == value.size()) return true;
  if (value[index++] != '.' || index == value.size()) return false;
  for (; index < value.size(); ++index)
    if (value[index] < '0' || value[index] > '9') return false;
  if (value[0] == '1')
    for (std::size_t i = 2; i < value.size(); ++i)
      if (value[i] != '0') return false;
  return true;
}
}  // namespace detail

inline nlohmann::json profile_json(const Profile& profile) {
  nlohmann::json out = profile.document.is_object()
                           ? profile.document
                           : nlohmann::json::object();
  out["name"] = profile.name;
  out["tree"] = profile.tree;
  if (out.contains("style") || !profile.document.is_object() ||
      !detail::style_json(profile.style).empty())
    out["style"] = detail::style_json(profile.style);
  if (out.contains("structure")) {
    out["structure"]["structure_layers"] = profile.structure_layers;
    if (profile.light_funnel_from)
      out["structure"]["light_funnel_from"] = *profile.light_funnel_from;
    if (!profile.imported_layers.empty())
      out["structure"]["imported_layers"] = profile.imported_layers;
  } else {
    out["structure_layers"] = profile.structure_layers;
  }
  return out;
}

inline std::optional<Profile> parse_profile(const nlohmann::json& value,
                                            std::string* error) {
  const auto fail = [&](const std::string& what) {
    if (error) *error = what;
    return std::optional<Profile>{};
  };
  if (!value.is_object()) return fail("profile must be a JSON object");
  if (value.dump().size() > detail::kMaxProfileBytes)
    return fail("profile is larger than 256 KiB");
  if (!detail::only_keys(value,
                         {"name", "tree", "style", "structure_layers",
                          "description", "persona", "system_prompt", "levels",
                          "structure", "atomic", "style_levels", "prompts",
                          "critique_anchors", "placeholders", "engine_contract_notes"},
                         "profile", error))
    return std::nullopt;
  if (!value.contains("name") || !value["name"].is_string() ||
      value["name"].get<std::string>().empty())
    return fail("profile name (non-empty string) is required");
  Profile profile;
  profile.name = value["name"].get<std::string>();
  if (value.contains("tree")) {
    if (!value["tree"].is_string()) return fail("profile tree must be a string");
    profile.tree = value["tree"].get<std::string>();
    // "planning" is the export filter name for the planning tree (tree "").
    if (profile.tree == "planning")
      return fail("tree name \"planning\" is reserved for the planning tree");
  }
  for (const auto entry : {std::pair<const char*, std::string*>{"description", &profile.description},
                           {"persona", &profile.persona},
                           {"system_prompt", &profile.system_prompt}}) {
    const auto it = value.find(entry.first);
    if (it == value.end()) continue;
    if (!it->is_string()) return fail(std::string("profile ") + entry.first + " must be a string");
    *entry.second = it->get<std::string>();
  }
  if (value.contains("structure_layers") && value.contains("structure"))
    return fail("profile cannot contain both structure_layers and structure");
  if (value.contains("structure_layers") &&
      !detail::parse_int_layers(value["structure_layers"], &profile.structure_layers,
                                "structure_layers", error))
    return std::nullopt;
  if (value.contains("structure")) {
    const auto& structure = value["structure"];
    if (!structure.is_object()) return fail("structure must be an object");
    if (!detail::only_keys(structure,
                           {"structure_layers", "imported_layers", "light_funnel_from"},
                           "structure", error))
      return std::nullopt;
    if (!structure.contains("structure_layers"))
      return fail("structure.structure_layers is required");
    if (!detail::parse_int_layers(structure["structure_layers"],
                                  &profile.structure_layers,
                                  "structure.structure_layers", error))
      return std::nullopt;
    if (structure.contains("imported_layers") &&
        !detail::parse_int_layers(structure["imported_layers"],
                                  &profile.imported_layers,
                                  "structure.imported_layers", error))
      return std::nullopt;
    if (structure.contains("light_funnel_from")) {
      if (!structure["light_funnel_from"].is_number_integer())
        return fail("structure.light_funnel_from must be an integer");
      const std::int64_t level = structure["light_funnel_from"].get<std::int64_t>();
      if (level < 0 || level > 9)
        return fail("structure.light_funnel_from out of range 0..9");
      profile.light_funnel_from = static_cast<int>(level);
    }
  }
  if (value.contains("style") &&
      !detail::parse_style(value["style"], &profile.style, "style", error))
    return std::nullopt;
  if (value.contains("levels")) {
    const auto& levels = value["levels"];
    if (!levels.is_object()) return fail("levels must be an object");
    if (!detail::only_keys(levels,
                           {"max", "label_format", "import_csv_pattern",
                            "import_codes", "definitions"}, "levels", error))
      return std::nullopt;
    if (!levels.contains("max") || !levels["max"].is_number_integer())
      return fail("levels.max (integer) is required");
    const std::int64_t maximum = levels["max"].get<std::int64_t>();
    if (maximum < 0 || maximum > 9) return fail("levels.max out of range 0..9");
    profile.max_level = static_cast<int>(maximum);
    for (const char* key : {"label_format", "import_csv_pattern"})
      if (levels.contains(key) && !levels[key].is_string())
        return fail(std::string("levels.") + key + " must be a string");
    if (levels.contains("import_codes")) {
      const auto& codes = levels["import_codes"];
      if (!codes.is_object()) return fail("levels.import_codes must be an object");
      for (auto it = codes.begin(); it != codes.end(); ++it)
        if (!it.value().is_string())
          return fail("levels.import_codes values must be strings");
    }
    if (!levels.contains("definitions") || !levels["definitions"].is_array())
      return fail("levels.definitions (array) is required");
    std::unordered_set<int> seen_levels;
    for (const auto& entry : levels["definitions"]) {
      if (!entry.is_object()) return fail("levels.definitions entries must be objects");
      if (!detail::only_keys(entry,
                             {"level", "name", "source", "children_are",
                              "children", "ordered_children"},
                             "levels.definitions entry", error))
        return std::nullopt;
      if (!entry.contains("level") || !entry["level"].is_number_integer() ||
          !entry.contains("name") || !entry["name"].is_string() ||
          !entry.contains("children_are") || !entry["children_are"].is_string())
        return fail("level definition needs integer level, name, and children_are");
      const std::int64_t level = entry["level"].get<std::int64_t>();
      if (level < 0 || level > maximum ||
          !seen_levels.insert(static_cast<int>(level)).second)
        return fail("level definitions must have unique levels within levels.max");
      LevelDefinition definition;
      definition.level = static_cast<int>(level);
      definition.name = entry["name"].get<std::string>();
      definition.children_are = entry["children_are"].get<std::string>();
      if (entry.contains("source")) {
        if (!entry["source"].is_string()) return fail("level definition source must be a string");
        definition.source = entry["source"].get<std::string>();
      }
      if (entry.contains("ordered_children")) {
        if (!entry["ordered_children"].is_boolean())
          return fail("level definition ordered_children must be a boolean");
        definition.ordered_children = entry["ordered_children"].get<bool>();
      }
      if (entry.contains("children")) {
        const auto& children = entry["children"];
        if (!children.is_object() ||
            !detail::only_keys(children, {"min", "max"}, "children", error) ||
            !children.contains("min") || !children["min"].is_number_integer() ||
            !children.contains("max") || !children["max"].is_number_integer())
          return fail("level children requires integer min and max");
        const std::int64_t minimum = children["min"].get<std::int64_t>();
        const std::int64_t maximum_children = children["max"].get<std::int64_t>();
        if (minimum < 1 || maximum_children < minimum || maximum_children > 100)
          return fail("level children range must be 1..100 with min <= max");
        definition.children = ChildRange{static_cast<int>(minimum),
                                         static_cast<int>(maximum_children)};
      }
      profile.levels.push_back(std::move(definition));
    }
  }
  if (value.contains("style_levels")) {
    if (!value["style_levels"].is_array()) return fail("style_levels must be an array");
    std::unordered_set<int> styled_levels;
    for (const auto& entry : value["style_levels"]) {
      if (!entry.is_object() || !entry.contains("levels"))
        return fail("style_levels entries require levels");
      StyleLevel style_level;
      if (!detail::parse_int_layers(entry["levels"], &style_level.levels,
                                    "style_levels.levels", error) ||
          style_level.levels.empty())
        return fail("style_levels.levels must be a non-empty integer array");
      for (const int layer : style_level.levels)
        if (!styled_levels.insert(layer).second)
          return fail("a child level may appear in only one style_levels entry");
      if (!detail::parse_style(entry, &style_level.style,
                               "style_levels entry", error, true))
        return std::nullopt;
      profile.style_levels.push_back(std::move(style_level));
    }
  }
  if (value.contains("atomic")) {
    const auto& atomic = value["atomic"];
    if (!atomic.is_object() ||
        !detail::only_keys(atomic, {"definition", "verify", "expected_from_level"},
                           "atomic", error))
      return fail("atomic must be an object");
    if (atomic.contains("definition")) {
      if (!atomic["definition"].is_string()) return fail("atomic.definition must be a string");
      profile.atomic_definition = atomic["definition"].get<std::string>();
    }
    if (atomic.contains("verify")) {
      if (!atomic["verify"].is_boolean()) return fail("atomic.verify must be a boolean");
      profile.atomic_verify = atomic["verify"].get<bool>();
    }
    if (atomic.contains("expected_from_level")) {
      if (!atomic["expected_from_level"].is_number_integer())
        return fail("atomic.expected_from_level must be an integer");
      const std::int64_t level = atomic["expected_from_level"].get<std::int64_t>();
      if (level < 0 || level > 9) return fail("atomic.expected_from_level out of range 0..9");
      profile.atomic_expected_from_level = static_cast<int>(level);
    }
  }
  if (value.contains("prompts")) {
    const auto& prompts = value["prompts"];
    if (!prompts.is_object() ||
        !detail::only_keys(prompts,
                           {"precheck", "planner", "worker", "trimmer", "critique",
                            "synthesizer", "atomic_verification"},
                           "prompts", error))
      return fail("prompts must be an object");
    // A profile that supplies prompts supplies all of them: a missing stage
    // would silently fall back to the planning-domain wording.
    for (const detail::PromptContract& contract : detail::prompt_contracts()) {
      const std::string name(contract.name);
      if (!prompts.contains(name))
        return fail("prompts." + name + " is required");
      if (!prompts[name].is_string())
        return fail("prompt templates must be strings");
      const std::string text = prompts[name].get<std::string>();
      if (text.empty() || text.size() > detail::kMaxPromptBytes)
        return fail("prompts." + name + " must be 1.." +
                    std::to_string(detail::kMaxPromptBytes) + " bytes");
      const std::vector<std::string> used = detail::placeholders_in(text);
      for (const std::string& marker : used) {
        const bool allowed =
            std::find(detail::common_placeholders().begin(),
                      detail::common_placeholders().end(),
                      marker) != detail::common_placeholders().end() ||
            std::find(contract.extra.begin(), contract.extra.end(), marker) !=
                contract.extra.end();
        if (!allowed)
          return fail("prompts." + name + " uses {" + marker +
                      "}, which that stage is never given");
      }
      for (const std::string_view needed : contract.required)
        if (std::find(used.begin(), used.end(), std::string(needed)) ==
            used.end())
          return fail("prompts." + name + " must contain {" +
                      std::string(needed) + "}");
    }
  }
  if (value.contains("critique_anchors")) {
    const auto& anchors = value["critique_anchors"];
    if (!anchors.is_array()) return fail("critique_anchors must be an array");
    for (const auto& anchor : anchors) {
      if (!anchor.is_object() ||
          !detail::only_keys(anchor, {"score", "node", "children", "why"},
                             "critique anchor", error))
        return fail("critique anchors require score, node, children, and why");
      if (!anchor.contains("score") || !anchor["score"].is_string() ||
          !detail::unit_decimal(anchor["score"].get<std::string>()))
        return fail("critique anchor score must be exact decimal text in 0..1");
      if (!anchor.contains("node") || !anchor["node"].is_string() ||
          !anchor.contains("children") ||
          !detail::validate_string_array(anchor["children"],
                                         "critique anchor children", error) ||
          !anchor.contains("why") || !anchor["why"].is_string())
        return fail("critique anchors require score, node, children, and why");
    }
  }
  if (value.contains("placeholders")) {
    if (!detail::validate_string_array(value["placeholders"], "placeholders",
                                       error))
      return std::nullopt;
    for (const auto& name : value["placeholders"])
      if (!detail::placeholder_known(name.get<std::string>()))
        return fail("placeholders lists {" + name.get<std::string>() +
                    "}, which the engine never fills");
  }
  if (value.contains("engine_contract_notes") &&
      !detail::validate_string_array(value["engine_contract_notes"],
                                     "engine_contract_notes", error))
    return std::nullopt;
  if (value.contains("style_levels")) {
    if (!value.contains("prompts")) return fail("style_levels requires prompts");
    for (const char* key : {"worker", "critique", "synthesizer"})
      if (!value["prompts"].contains(key) ||
          value["prompts"][key].get<std::string>().find("{title_rules}") ==
              std::string::npos)
        return fail("style_levels prompts must render {title_rules}");
  }
  // Cross-field coherence: a profile that declares levels must describe one
  // consistent ladder, or a campaign would run on rules that contradict
  // each other (a range for children that are imported, a title rule for a
  // level the engine never generates, a layer past levels.max).
  if (profile.max_level >= 0) {
    const auto in = [](const std::vector<int>& layers, int layer) {
      return std::find(layers.begin(), layers.end(), layer) != layers.end();
    };
    for (const LevelDefinition& definition : profile.levels) {
      if (!definition.source.empty() && definition.source != "import")
        return fail("level " + std::to_string(definition.level) +
                    " source must be \"import\" or absent");
      const bool says_import = definition.source == "import";
      if (says_import != in(profile.imported_layers, definition.level))
        return fail("level " + std::to_string(definition.level) +
                    " source disagrees with structure.imported_layers");
      // A range or an order only means something for children the engine
      // generates; children of an imported level are imported too.
      const bool children_generated =
          !in(profile.imported_layers, definition.level + 1);
      if (!children_generated &&
          (definition.children || definition.ordered_children))
        return fail("level " + std::to_string(definition.level) +
                    " children are imported, so it cannot declare a range "
                    "or ordered_children");
    }
    const auto within = [&](int layer) {
      return layer >= 0 && layer <= profile.max_level;
    };
    for (const int layer : profile.structure_layers)
      if (!within(layer)) return fail("structure_layers beyond levels.max");
    for (const int layer : profile.imported_layers)
      if (!within(layer)) return fail("imported_layers beyond levels.max");
    if (profile.light_funnel_from && !within(*profile.light_funnel_from))
      return fail("structure.light_funnel_from beyond levels.max");
    if (profile.atomic_expected_from_level &&
        !within(*profile.atomic_expected_from_level))
      return fail("atomic.expected_from_level beyond levels.max");
    for (const StyleLevel& style_level : profile.style_levels)
      for (const int layer : style_level.levels) {
        if (!within(layer) || layer == 0)
          return fail("style_levels names level " + std::to_string(layer) +
                      " outside 1..levels.max");
        if (in(profile.imported_layers, layer))
          return fail("style_levels names imported level " +
                      std::to_string(layer) +
                      ", whose titles the engine never generates");
      }
  }
  profile.document = value;
  // The record that is signed and folded is profile_json(), which adds
  // fields (tree, structure_layers, style) to the input. The cap must hold
  // for THAT form, or a profile that publishes cleanly would be refused when
  // every device re-parses it at fold time.
  if (profile_json(profile).dump().size() > detail::kMaxProfileBytes)
    return fail("profile is larger than 256 KiB");
  return profile;
}

inline std::string profile_hash(const Profile& profile) {
  return onion::log::blake2b_hex(
      onion::log::canonical_json(profile_json(profile)));
}

}  // namespace onion::domain
