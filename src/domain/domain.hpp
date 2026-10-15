#pragma once

#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "log/log.hpp"

namespace onion::domain {

// Decision 0027: domain profiles. One signed profile record per tree,
// distributed with the log; the campaign record carries the profile's
// hash, and a device that does not hold that exact profile does not run
// the campaign. The planning profile is embedded and stays the default,
// byte-identical in behaviour.
//
// The deterministic style checks (Title Case, word limits, punctuation)
// live here — in the profile — not in hard-coded globals.
//
// Header-only on purpose: depot_core needs the planning profile without
// pulling log into the base layer.

struct StyleChecks {
  // Unset fields mean "no check" (a profile that only limits words, say).
  bool title_case{false};
  int max_words{0};               // 0 = no limit
  bool no_questions{false};       // '?' and '!' banned
  bool no_trailing_punct{false};  // trailing '.' or ':' banned
  std::vector<std::string> filler_words;
};

struct Profile {
  std::string name;
  std::string tree;  // "" = the planning tree
  StyleChecks style;
  // The layers that need the structure permission; without it a commit
  // at one of these layers goes to review instead.
  std::vector<int> structure_layers;
};

// The embedded planning profile: exactly today's hard-coded behaviour
// (Title Case, 2..8 words, no questions/bangs, no trailing punctuation,
// the current filler list, structure allowed at layers 0..2).
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

// Style flags for one title under the given checks; empty = clean. The
// planning profile produces the historical flag set exactly.
inline std::vector<std::string> style_flags(std::string_view title,
                                            const StyleChecks& checks) {
  std::vector<std::string> f;
  const std::string text(title);
  if (checks.no_questions && (text.find('?') != std::string::npos ||
                              text.find('!') != std::string::npos))
    f.push_back("question_or_bang");
  if (checks.title_case && (text.empty() ||
      !std::isupper(static_cast<unsigned char>(text[0]))))
    f.push_back("not_title_case");
  std::size_t words = 0;
  {
    std::istringstream ss(text);
    std::string w;
    while (ss >> w) ++words;
  }
  if (checks.max_words > 0 && static_cast<int>(words) > checks.max_words)
    f.push_back("too_many_words");
  if (checks.no_trailing_punct && !text.empty() &&
      (text.back() == '.' || text.back() == ':'))
    f.push_back("trailing_punct");
  if (words > 2) {
    const std::unordered_set<std::string> fillers(checks.filler_words.begin(),
                                                  checks.filler_words.end());
    std::istringstream s2(text);
    std::string x;
    bool filler = false;
    while (s2 >> x) {
      std::string lowered;
      for (const unsigned char c : x)
        lowered.push_back(static_cast<char>(std::tolower(c)));
      if (fillers.count(lowered)) filler = true;
    }
    if (filler) f.push_back("contains_filler");
  }
  return f;
}

// Canonical JSON form of a profile (profile records and the campaign's
// hash both use it).
inline nlohmann::json profile_json(const Profile& profile) {
  nlohmann::json out{
      {"name", profile.name},
      {"tree", profile.tree},
      {"structure_layers", profile.structure_layers},
  };
  nlohmann::json style = nlohmann::json::object();
  if (profile.style.title_case) style["title_case"] = true;
  if (profile.style.max_words > 0) style["max_words"] = profile.style.max_words;
  if (profile.style.no_questions) style["no_questions"] = true;
  if (profile.style.no_trailing_punct) style["no_trailing_punct"] = true;
  if (!profile.style.filler_words.empty())
    style["filler_words"] = profile.style.filler_words;
  out["style"] = std::move(style);
  return out;
}

inline std::optional<Profile> parse_profile(const nlohmann::json& value,
                                            std::string* error) {
  const auto fail = [&](const std::string& what) {
    if (error) *error = what;
    return std::nullopt;
  };
  if (!value.is_object()) return fail("profile must be a JSON object");
  if (!value.contains("name") || !value["name"].is_string())
    return fail("profile name (string) is required");
  Profile profile;
  profile.name = value["name"].get<std::string>();
  profile.tree = value.value("tree", std::string{});
  if (value.contains("structure_layers")) {
    if (!value["structure_layers"].is_array())
      return fail("structure_layers must be an array of integers");
    for (const auto& layer : value["structure_layers"]) {
      if (!layer.is_number_integer())
        return fail("structure_layers entries must be integers");
      const std::int64_t v = layer.get<std::int64_t>();
      if (v < 0 || v > 9) return fail("structure_layers out of range 0..9");
      profile.structure_layers.push_back(static_cast<int>(v));
    }
  }
  if (value.contains("style")) {
    const auto& style = value["style"];
    if (!style.is_object()) return fail("style must be an object");
    profile.style.title_case = style.value("title_case", false);
    if (style.contains("max_words")) {
      if (!style["max_words"].is_number_integer())
        return fail("style.max_words must be an integer");
      const std::int64_t v = style["max_words"].get<std::int64_t>();
      if (v < 1 || v > 100) return fail("style.max_words out of range 1..100");
      profile.style.max_words = static_cast<int>(v);
    }
    profile.style.no_questions = style.value("no_questions", false);
    profile.style.no_trailing_punct = style.value("no_trailing_punct", false);
    if (style.contains("filler_words")) {
      if (!style["filler_words"].is_array())
        return fail("style.filler_words must be an array of strings");
      for (const auto& word : style["filler_words"]) {
        if (!word.is_string())
          return fail("filler_words entries must be strings");
        profile.style.filler_words.push_back(word.get<std::string>());
      }
    }
  }
  return profile;
}

// BLAKE2b-256 (hex) over canonical_json(profile_json(profile)) — the
// value a campaign record carries.
inline std::string profile_hash(const Profile& profile) {
  return onion::log::blake2b_hex(onion::log::canonical_json(profile_json(profile)));
}

}  // namespace onion::domain
