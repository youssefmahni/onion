#include "gate.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/depot_core.hpp"

namespace onion::gate {
namespace {

std::string node_line(const NodeContext& ctx) {
  return "Node: L" + std::to_string(ctx.layer) + " " + ctx.path + "\nTitle: " +
         ctx.title;
}

std::string join(const std::vector<std::string>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i) out += '|';
    out += items[i];
  }
  return out;
}

std::string existing_line(const NodeContext& ctx) {
  if (ctx.existing.empty()) return {};
  return "Existing children (do NOT repeat any of these): " + join(ctx.existing);
}

bool result_ok(const llm::Result& result) {
  return result.kind == llm::Kind::Ok;
}

// "" for a reply the funnel can judge (Ok, or a model-quality failure such
// as Truncated/Malformed); otherwise the provider-failure class.
std::string provider_failure_class(llm::Kind kind) {
  switch (kind) {
    case llm::Kind::Transport:
    case llm::Kind::Timeout:
    case llm::Kind::ServerError:
    case llm::Kind::RateLimited:
      return "transient";
    case llm::Kind::AuthError:
      return "auth";
    case llm::Kind::BalanceError:
      return "balance";
    case llm::Kind::ClientError:
      return "rejected";
    default:
      return "";
  }
}

}  // namespace

std::string system_prompt() {
  return "You are a strict knowledge-curation assistant for a hierarchical "
         "work-breakdown knowledge base that powers AI agents. Do not reason "
         "out loud and do not write any explanation — answer ONLY with the "
         "JSON object requested, nothing else.";
}

std::string precheck_prompt(const NodeContext& ctx) {
  std::ostringstream out;
  // Fixed text first, node-specific text last (prompt-cache prefix).
  out << "This knowledge node must be decomposed into child items for an "
         "AI-agent knowledge base.\n"
      << "Can this node be split into at least 3 distinct parts, each part a "
         "meaningful direct component of the node itself? Answer ONLY with "
         "JSON {\"splits\": true} or {\"splits\": false}.\n"
      << node_line(ctx);
  return out.str();
}

std::string planner_prompt(const NodeContext& ctx,
                           const std::string& injected_issues, bool light,
                           int aspects_min, int aspects_max) {
  std::ostringstream out;
  // Fixed text first, node-specific text last (prompt-cache prefix).
  out << "Plan the knowledge coverage for this node: list "
      << (light ? "2-3" : (std::to_string(aspects_min) + "-" +
                           std::to_string(aspects_max)))
      << " aspects. Every aspect must be a dimension OF THE NODE ITSELF "
         "(things it directly contains or decomposes into) — never related "
         "topics from other domains. Return JSON {\"items\": [ aspects ]}.\n";
  if (!injected_issues.empty())
    out << "Problems with your previous attempt that you MUST fix: "
        << injected_issues << "\n";
  const std::string existing = existing_line(ctx);
  if (!existing.empty()) out << existing << "\n";
  out << node_line(ctx);
  return out.str();
}

std::string worker_prompt(const NodeContext& ctx, const std::string& aspect,
                          const std::string& existing_joined,
                          int candidates, bool compact) {
  std::ostringstream out;
  // Fixed text first, node-specific text last (prompt-cache prefix).
  out << "List the top " << candidates
      << " child items of this node for this aspect in an "
         "AI-agent knowledge base. Every item MUST be a direct child of \""
      << ctx.title
      << "\" within its exact scope — do NOT include related topics, industry "
         "classifications, or the node itself. Title Case noun phrases, 2-5 "
         "words, no duplicates, no questions, no emoji. Return JSON "
      << (compact ? "{\"titles\": [ ... up to " + std::to_string(candidates) +
                        " ... ]}"
                  : "{\"items\": [ ... up to " + std::to_string(candidates) +
                        " ... ]}")
      << ".\n"
      << "Aspect: " << aspect << "\n";
  if (!existing_joined.empty())
    out << "Existing children (do NOT repeat): " << existing_joined << "\n";
  out << node_line(ctx);
  return out.str();
}

std::string trimmer_prompt(const NodeContext& ctx,
                           const std::vector<std::string>& candidates,
                           int keep) {
  std::ostringstream out;
  out << "From these candidates keep only the best " << keep
      << " — drop the weakest, you may re-rank. Return the titles VERBATIM, "
         "do not rephrase them. Candidates:\n"
      << join(candidates) << "\nReturn JSON {\"items\": [ ... exactly " << keep
      << " ... ]}.\n"
      << node_line(ctx);
  return out.str();
}

std::string critique_prompt(const NodeContext& ctx,
                            const std::vector<std::string>& candidates) {
  std::ostringstream out;
  out << "Critique this candidate set for the node. This is a RAW CANDIDATE "
         "POOL meant to be filtered: score it on how many candidates are "
         "usable direct children of \""
      << ctx.title
      << "\" (precision), not on whether every candidate is perfect. Check: "
         "(1) hallucinations or off-topic items, (2) duplicates, (3) "
         "important missing categories, (4) weak or vague items, (5) "
         "compliance (Title Case, no questions, no emoji).\n"
         // Anchored rubric (decision 0020): the score is calibrated
         // against these fixed example anchors, so the 0.0-1.0 scale
         // means the same thing across calls and models.
         "Calibrate against these anchors for the node \"Order "
         "Capture\":\n"
         "- Score 0.8 example: [Order Entry Form, Payment Authorization, "
         "Shipping Method Selection, Order Confirmation, Order Status "
         "Tracking] — every item is a direct part, none weak.\n"
         "- Score 0.5 example: [Order Entry Form, Payment Authorization, "
         "Discount Codes, Customer Loyalty Program, Refund Policy] — the "
         "first two are direct parts, the rest belong to sibling nodes.\n"
         "- Score 0.2 example: [Selling Strategies, Customer Service, Best "
         "Practices, E-commerce Trends, Tips And Tricks] — mostly "
         "off-topic.\n"
         "Apply the same standard to this node. Candidates:\n"
      << join(candidates)
      << "\nReturn JSON {\"score\": 0.0-1.0 usable-candidate ratio, "
         "\"remove\": [ items to drop ], \"add\": [ missing items to include "
         "], \"issues\": \"short list of problems\"}.\n"
      << node_line(ctx);
  return out.str();
}

std::string synthesizer_prompt(const NodeContext& ctx,
                               const std::vector<std::string>& candidates,
                               const std::string& critique_text,
                               int target_min, int target_max,
                               int max_children, bool compact,
                               const std::string& problems) {
  std::ostringstream out;
  out << "From these candidates select the final children set for this node. "
      << (critique_text.empty()
              ? "Drop hallucinated or weak items and resolve any conflicts. "
              : "Incorporate the critique: drop hallucinated or weak items, "
                "include the missing items you judge valid, resolve any "
                "conflicts. ")
      << "Keep between "
      << target_min << " and " << target_max
      << " children (never more than " << max_children
      << "). Do not pad — keep only what the node genuinely contains. ";
  if (compact) {
    out << "Return JSON {\"titles\": [ kept titles VERBATIM ], "
           "\"atomic_indices\": [ 0-based indexes into titles for the items "
           "that cannot be decomposed into simpler items ]}. ";
  } else {
    out << "For each kept item "
           "return {\"title\": \"...\", \"atomic\": true if it cannot be "
           "decomposed into simpler items, false if it should nest further}. ";
  }
  out << "Candidates:\n" << join(candidates) << "\n";
  if (!critique_text.empty()) out << "Critique: " << critique_text << "\n";
  if (!problems.empty())
    out << "Problems with your previous attempt that you MUST fix: "
        << problems << "\n";
  out << node_line(ctx);
  return out.str();
}

std::string atomic_verification_prompt(
    const std::string& node_title,
    const std::vector<std::string>& labelled_children) {
  std::ostringstream out;
  out << "The node \"" << node_title
      << "\" was expanded and these children were labelled atomic (cannot "
         "be decomposed further):\n";
  for (const std::string& child : labelled_children) out << "- " << child << "\n";
  out << "Verify each against the node: which of these admit NO further "
         "decomposition into simpler sub-items for an AI-agent knowledge "
         "base? Answer ONLY with JSON {\"admits_none\": [ their titles "
         "VERBATIM ]}.";
  return out.str();
}

std::optional<std::vector<std::string>> parse_items(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object()) return std::nullopt;
  const char* key = json.contains("items") ? "items"
                    : json.contains("titles") ? "titles"
                                              : nullptr;
  if (key == nullptr || !json[key].is_array()) return std::nullopt;
  std::vector<std::string> items;
  for (const auto& item : json[key]) {
    if (!item.is_string()) return std::nullopt;
    items.push_back(item.get<std::string>());
  }
  return items;
}

std::optional<std::vector<Child>> parse_children(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object()) return std::nullopt;
  // The prompt asks for "items"; the model drifts between "items" and
  // "children" (recorded in the W5.4 cassettes — both shapes appear).
  // Both object-array shapes are accepted; the entries stay strict.
  const char* object_key = json.contains("items") ? "items"
                           : json.contains("children") ? "children"
                                                       : nullptr;
  if (object_key != nullptr && json[object_key].is_array()) {
    std::vector<Child> children;
    for (const auto& item : json[object_key]) {
      if (!item.is_object() || !item.contains("title") ||
          !item["title"].is_string())
        return std::nullopt;
      Child child;
      child.title = item["title"].get<std::string>();
      child.atomic = item.contains("atomic") && item["atomic"].is_boolean() &&
                     item["atomic"].get<bool>();
      children.push_back(std::move(child));
    }
    return children;
  }
  // Compact (V5): titles array + atomic_indices.
  if (json.contains("titles") && json["titles"].is_array()) {
    std::vector<Child> children;
    for (const auto& title : json["titles"]) {
      if (!title.is_string()) return std::nullopt;
      Child child;
      child.title = title.get<std::string>();
      children.push_back(std::move(child));
    }
    if (json.contains("atomic_indices")) {
      if (!json["atomic_indices"].is_array()) return std::nullopt;
      for (const auto& index : json["atomic_indices"]) {
        if (!index.is_number_integer()) return std::nullopt;
        const auto i = index.get<std::int64_t>();
        if (i < 0 || static_cast<std::size_t>(i) >= children.size())
          return std::nullopt;
        children[static_cast<std::size_t>(i)].atomic = true;
      }
    }
    return children;
  }
  return std::nullopt;
}

std::optional<Critique> parse_critique(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object() || !json.contains("score") ||
      !json["score"].is_number())
    return std::nullopt;
  Critique critique;
  critique.score = json["score"].get<double>();
  // The prompt asks for 0.0-1.0 (usable-candidate ratio).  A score outside
  // that range means the model drifted scale (0-10, 0-100, percent) and
  // the attempt must fail rather than silently mis-score the gate — W5
  // gate calibration finding (DR 0018).
  if (!std::isfinite(critique.score) || critique.score < 0.0 ||
      critique.score > 1.0)
    return std::nullopt;
  if (json.contains("issues") && json["issues"].is_string())
    critique.issues = json["issues"].get<std::string>();
  if (json.contains("remove") && json["remove"].is_array())
    for (const auto& item : json["remove"])
      if (item.is_string()) critique.remove.push_back(item.get<std::string>());
  if (json.contains("add") && json["add"].is_array())
    for (const auto& item : json["add"])
      if (item.is_string()) critique.add.push_back(item.get<std::string>());
  return critique;
}

std::optional<bool> parse_precheck(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object() || !json.contains("splits") ||
      !json["splits"].is_boolean())
    return std::nullopt;
  return json["splits"].get<bool>();
}

std::optional<std::vector<std::string>> parse_atomic_verdict(
    const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object() ||
      !json.contains("admits_none") || !json["admits_none"].is_array())
    return std::nullopt;
  std::vector<std::string> admits_none;
  for (const auto& item : json["admits_none"]) {
    if (!item.is_string()) return std::nullopt;
    admits_none.push_back(item.get<std::string>());
  }
  return admits_none;
}

bool Checks::passed() const {
  return count_ok && no_repeats && style_ok && ancestors_ok;
}

Checks run_checks(const std::vector<Child>& children, const NodeContext& ctx,
                  int max_children, int min_children,
                  const onion::domain::StyleChecks* style) {
  Checks checks;
  checks.count_ok =
      children.size() >= static_cast<std::size_t>(min_children) &&
      children.size() <= static_cast<std::size_t>(max_children);
  if (!checks.count_ok)
    checks.failures.push_back("child count " + std::to_string(children.size()) +
                              " outside " + std::to_string(min_children) +
                              ".." + std::to_string(max_children));

  std::set<std::string> seen;
  seen.insert(depot::canonical(ctx.title));
  for (const std::string& ancestor : ctx.ancestors)
    seen.insert(depot::canonical(ancestor));
  for (const std::string& sibling : ctx.siblings)
    seen.insert(depot::canonical(sibling));
  for (const std::string& existing : ctx.existing)
    seen.insert(depot::canonical(existing));

  checks.no_repeats = true;
  checks.style_ok = true;
  checks.ancestors_ok = true;
  checks.child_valid.assign(children.size(), 1);
  const std::string node_canonical = depot::canonical(ctx.title);
  for (std::size_t index = 0; index < children.size(); ++index) {
    const Child& child = children[index];
    const std::string canonical = depot::canonical(child.title);
    if (!seen.insert(canonical).second) {
      checks.no_repeats = false;
      checks.child_valid[index] = 0;
      checks.failures.push_back("repeated title: " + child.title);
    }
    // Decision 0027: the style rules come from the domain profile
    // (nullptr = the embedded planning profile's checks).
    const auto flags = style
                           ? onion::domain::style_flags(child.title, *style)
                           : depot::style_flags(child.title);
    if (!flags.empty()) {
      checks.style_ok = false;
      checks.child_valid[index] = 0;
      checks.failures.push_back("style violation: " + child.title);
    }
    // Parent similarity ≥ 0.85 → reject; any higher ancestor ≥ 0.95 → reject.
    if (depot::sim_keys(canonical, node_canonical) >= 0.85) {
      checks.ancestors_ok = false;
      checks.child_valid[index] = 0;
      checks.failures.push_back("too similar to parent: " + child.title);
    }
    for (const std::string& ancestor : ctx.ancestors) {
      if (depot::sim_keys(canonical, depot::canonical(ancestor)) >= 0.95) {
        checks.ancestors_ok = false;
        checks.child_valid[index] = 0;
        checks.failures.push_back("too similar to ancestor: " + child.title);
        break;
      }
    }
  }
  for (const char valid : checks.child_valid)
    if (valid) ++checks.valid_count;
  return checks;
}

// Decision 0024: the gate is the deterministic checks.
Outcome decide(const std::vector<Attempt>& attempts, const GateParams& params) {
  Outcome outcome;
  // The first attempt whose checks all pass commits.
  for (std::size_t i = 0; i < attempts.size(); ++i) {
    if (attempts[i].checks_passed) {
      outcome.decision = Decision::Commit;
      outcome.chosen = static_cast<int>(i);
      outcome.end_reason = "children";
      return outcome;
    }
  }
  // Retry only when a check failed.
  if (static_cast<int>(attempts.size()) < params.max_attempts) {
    outcome.decision = Decision::Retry;
    outcome.end_reason = "retry";
    return outcome;
  }
  // Attempts exhausted: the attempt with the most valid children (ties: the
  // later one) commits when it has at least `fallback_min` valid children
  // (3 for the planning default, the profile range minimum for a domain
  // profile); otherwise the node is a leaf.
  int best = -1;
  std::size_t best_valid = 0;
  const std::size_t minimum =
      static_cast<std::size_t>(std::max(1, params.fallback_min));
  for (std::size_t i = 0; i < attempts.size(); ++i)
    if (attempts[i].valid_count >= minimum &&
        attempts[i].valid_count >= best_valid) {
      best = static_cast<int>(i);
      best_valid = attempts[i].valid_count;
    }
  if (best < 0) {
    outcome.decision = Decision::Leaf;
    outcome.end_reason = "cannot split";
    return outcome;
  }
  outcome.decision = Decision::Commit;
  outcome.chosen = best;
  outcome.end_reason = "children";
  outcome.below_target =
      best_valid < static_cast<std::size_t>(params.children_min);
  return outcome;
}

struct Funnel::Impl {
  FunnelOptions options;
};

Funnel::Funnel(FunnelOptions options)
    : impl_(std::make_unique<Impl>(Impl{options})) {}
Funnel::~Funnel() = default;
Funnel::Funnel(Funnel&&) noexcept = default;
Funnel& Funnel::operator=(Funnel&&) noexcept = default;

std::string add_usd(const std::string& a_in, const std::string& b_in) {
  std::string a = a_in;
  std::string b = b_in;
    std::size_t a_dot = a.find('.');
  std::size_t b_dot = b.find('.');
  const std::string a_int = a.substr(0, a_dot);
  const std::string b_int = b.substr(0, b_dot);
  std::string a_frac = a_dot == std::string::npos ? "0" : a.substr(a_dot + 1);
  std::string b_frac = b_dot == std::string::npos ? "0" : b.substr(b_dot + 1);
  const std::size_t frac = std::max(a_frac.size(), b_frac.size());
  a_frac.append(frac - a_frac.size(), '0');
  b_frac.append(frac - b_frac.size(), '0');
  const std::int64_t total =
      std::stoll(a_int + a_frac, nullptr, 10) +
      std::stoll(b_int + b_frac, nullptr, 10);
  const std::string text = std::to_string(total);
  std::string out_text;
  if (text.size() <= frac)
    out_text = "0." + std::string(frac - text.size(), '0') + text;
  else
    out_text = text.substr(0, text.size() - frac) + "." +
               text.substr(text.size() - frac);
  while (out_text.size() > 1 && out_text.back() == '0') out_text.pop_back();
  if (!out_text.empty() && out_text.back() == '.') out_text.pop_back();
  return out_text.empty() ? "0" : out_text;
}

namespace {

llm::Request make_request(const FunnelOptions& options, const NodeContext&,
                          std::string user, double temperature,
                          int max_tokens_override = 0) {
  llm::Request request;
  request.model = options.model;
  // A non-planning profile supplies the signed system instruction. The
  // embedded planning profile keeps the historical prompt byte-for-byte.
  request.system = options.profile.system_prompt.empty()
                       ? system_prompt()
                       : options.profile.system_prompt;
  if (!options.profile.persona.empty())
    request.system += "\n\n" + options.profile.persona;
  if (!options.cache_prefix.empty())
    request.user = options.cache_prefix + "\n" + user;
  else
    request.user = std::move(user);
  request.max_tokens = max_tokens_override ? max_tokens_override
                                           : options.max_tokens;
  request.temperature = temperature;
  return request;
}

// One left-to-right pass over the template: a `{name}` marker is replaced by
// its value and the inserted text is never rescanned, so a node title or
// candidate that itself contains `{siblings}` stays literal. Unknown markers
// are left as written (profile loading rejects them up front).
std::string render_template(
    const std::string& text,
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '{') {
      const std::size_t close = text.find('}', i + 1);
      if (close != std::string::npos) {
        const std::string name = text.substr(i + 1, close - i - 1);
        bool found = false;
        for (auto it = values.rbegin(); it != values.rend(); ++it)
          if (it->first == name) {
            out += it->second;
            found = true;
            break;
          }
        if (found) {
          i = close + 1;
          continue;
        }
      }
    }
    out += text[i++];
  }
  return out;
}

std::string prompt_value(const onion::domain::Profile& profile,
                         std::string_view prompt_name,
                         const NodeContext& ctx, int children_min,
                         int children_max,
                         const std::vector<std::pair<std::string, std::string>>&
                             extra = {}) {
  if (!profile.document.is_object() || !profile.document.contains("prompts") ||
      !profile.document["prompts"].contains(std::string(prompt_name)))
    return {};
  std::string output =
      profile.document["prompts"][std::string(prompt_name)].get<std::string>();
  std::string ancestors = join(ctx.ancestors);
  std::string siblings = join(ctx.siblings);
  std::string existing = join(ctx.existing);
  std::string anchors = "[]";
  if (profile.document.contains("critique_anchors"))
    anchors = profile.document["critique_anchors"].dump();
  const auto& style = onion::domain::style_for_child_level(profile, ctx.layer + 1);
  const std::vector<std::pair<std::string, std::string>> values = {
      {"title", ctx.title},
      {"path", ctx.path},
      {"level", std::to_string(ctx.layer)},
      {"level_name", onion::domain::level_name(profile, ctx.layer)},
      {"child_level_name", onion::domain::level_name(profile, ctx.layer + 1)},
      {"child_level_guidance",
       onion::domain::child_level_guidance(profile, ctx.layer)},
      {"ancestors", ancestors.empty() ? "none" : ancestors},
      {"siblings", siblings.empty() ? "none" : siblings},
      {"existing_children", existing.empty() ? "none" : existing},
      {"children_min", std::to_string(children_min)},
      {"children_max", std::to_string(children_max)},
      {"title_rules", onion::domain::title_rules(style)},
      {"atomic_definition", profile.atomic_definition.empty()
                                ? "a single indivisible operation"
                                : profile.atomic_definition},
      {"critique_anchors", anchors},
  };
  std::vector<std::pair<std::string, std::string>> all = values;
  all.insert(all.end(), extra.begin(), extra.end());
  output = render_template(output, all);
  // Profiles own prose, never parser keys. Keep every output contract in
  // code so a signed profile cannot widen what the strict parsers accept.
  if (prompt_name == "precheck")
    output += "\nReturn JSON {\"splits\": true} or {\"splits\": false}.";
  else if (prompt_name == "planner" || prompt_name == "worker" ||
           prompt_name == "trimmer")
    output += "\nReturn JSON {\"items\": [ strings ]}.";
  else if (prompt_name == "critique")
    output += "\nReturn JSON {\"score\": 0.0-1.0, \"remove\": [ strings ], "
              "\"add\": [ strings ], \"issues\": \"...\"}.";
  else if (prompt_name == "synthesizer")
    output += "\nReturn JSON {\"items\": [{\"title\": \"...\", "
              "\"atomic\": false}]}.";
  else if (prompt_name == "atomic_verification")
    output += "\nReturn JSON {\"admits_none\": [ titles verbatim ]}.";
  return output;
}

struct Meter {
  std::uint64_t in_hit{};
  std::uint64_t in_miss{};
  std::uint64_t out{};
  const llm::PriceSchedule* prices{};

  void add(const llm::Result& result) {
    in_hit += result.in_hit;
    in_miss += result.in_miss;
    out += result.out;
  }

  std::string cost() const {
    if (!prices) return "0";
    // (cache-miss in × in + cache-hit in × cache + out × out) / 1M, peak
    // triplet (the conservative maximum — decision 0004).
    const auto cents = [](const std::string& usd) -> std::int64_t {
      std::size_t dot = usd.find('.');
      const std::string integer = dot == std::string::npos ? usd : usd.substr(0, dot);
      std::string fraction = dot == std::string::npos ? "0" : usd.substr(dot + 1);
      if (fraction.size() < 9) fraction.append(9 - fraction.size(), '0');
      return std::stoll(integer + fraction.substr(0, 9), nullptr, 10);
    };
    // Tokens × USD-per-token, in integer micro-cents to stay exact.
    const auto mul = [&cents](std::uint64_t tokens, const std::string& usd) {
      return static_cast<long double>(tokens) * static_cast<long double>(cents(usd)) / 1000000.0L;
    };
    const long double total = mul(in_miss, prices->peak.input) +
                              mul(in_hit, prices->peak.cached_input) +
                              mul(out, prices->peak.output);
    // Round to nano-dollars (9 decimal places), like the rest of the ledger.
    std::ostringstream out_text;
    out_text << std::fixed << std::setprecision(9) << (total / 1e9L);
    return out_text.str();
  }
};

}  // namespace

FunnelResult Funnel::run(const NodeContext& ctx, const SyncAsk& ask) {
  FunnelResult result;
  // The provider-failure class of the most recent call, and the first one
  // that doomed the current attempt (F-GATE-2).
  std::string last_failure;
  std::string doomed_by;
  const SyncAsk counted = [&](const llm::Request& request) -> llm::Result {
    result.calls++;
    llm::Result reply = ask(request);
    last_failure = provider_failure_class(reply.kind);
    return reply;
  };
  const auto doom = [&]() {
    if (doomed_by.empty()) doomed_by = last_failure;
  };
  const auto profiled_prompt =
      [&](std::string_view name,
          const std::vector<std::pair<std::string, std::string>>& values,
          const std::string& fallback) {
        const std::string rendered = prompt_value(
            impl_->options.profile, name, ctx, impl_->options.params.children_min,
            impl_->options.synth_max_children, values);
        return rendered.empty() ? fallback : rendered;
      };

  // Pre-check: at most 400 tokens, temperature 0.
  {
    bool tried = false;
    std::string precheck_failure;
    for (int attempt = 0; attempt < 2; ++attempt) {
      const llm::Result precheck = counted(make_request(
          impl_->options, ctx,
          profiled_prompt("precheck", {}, precheck_prompt(ctx)), 0.0, 400));
      precheck_failure = last_failure;
      if (result_ok(precheck)) {
        tried = true;
        const auto splits = parse_precheck(precheck.content);
        if (splits && !*splits) {
          result.decision = Decision::Leaf;
          result.end_reason = "cannot split";
          result.precheck_leaf = true;
          return result;
        }
        break;
      }
    }
    // A pre-check that never got an answer because the provider failed is
    // not an answer: stop here rather than spend three doomed attempts.
    if (!tried && !precheck_failure.empty()) {
      result.decision = Decision::Defer;
      result.end_reason = "provider unavailable";
      result.provider_failure = precheck_failure;
      return result;
    }
    (void)tried;  // a failed pre-check runs the funnel; its checks decide
  }

  // The failure reasons of the previous attempt (decision 0024): injected
  // into the planner and the synthesizer of the next one.
  std::string injected_issues;
  std::vector<Attempt> attempts;
  std::vector<Meter> meters;
  attempts.reserve(3);
  meters.reserve(3);

  for (int number = 1; number <= impl_->options.params.max_attempts;
       ++number) {
    Attempt attempt;
    attempt.number = number;
    Meter meter;
    meter.prices = impl_->options.prices ? &*impl_->options.prices : nullptr;
    const double temperature =
        impl_->options.temperatures[(number - 1) % 3];
    bool failed = false;
    doomed_by.clear();

    // Planner.
    std::vector<std::string> aspects;
    {
      const llm::Result reply = counted(make_request(
          impl_->options, ctx,
          profiled_prompt(
              "planner",
              {{"n_aspects_min", std::to_string(impl_->options.light ? 2
                                                                       : impl_->options.aspects_min)},
               {"n_aspects_max", std::to_string(impl_->options.light ? 3
                                                                       : impl_->options.aspects_max)},
               {"retry_reasons", injected_issues.empty() ? "none" : injected_issues}},
              planner_prompt(ctx, injected_issues, impl_->options.light,
                             impl_->options.aspects_min,
                             impl_->options.aspects_max)),
          temperature));
      meter.add(reply);
      if (!result_ok(reply)) {
        failed = true;
        doom();
      } else {
        auto parsed = parse_items(reply.content);
        if (!parsed) failed = true;
        else {
          aspects = std::move(*parsed);
          const std::size_t cap =
              impl_->options.light ? 3
                                   : static_cast<std::size_t>(
                                         impl_->options.aspects_max);
          if (aspects.size() > cap) aspects.resize(cap);
        }
      }
    }

    // Workers, one call per aspect (sequential; the llm client itself is
    // multiplexed — M3 keeps the funnel call order deterministic).
    std::vector<std::string> candidates;
    if (!failed && !aspects.empty()) {
      const std::string existing_joined = join(ctx.existing);
      std::set<std::string> seen;
      for (const std::string& aspect : aspects) {
        const llm::Result reply = counted(make_request(
            impl_->options, ctx,
            profiled_prompt(
                "worker",
                {{"aspect", aspect},
                 {"n_items", std::to_string(impl_->options.worker_candidates)}},
                worker_prompt(ctx, aspect, existing_joined,
                              impl_->options.worker_candidates,
                              impl_->options.compact_output)),
            temperature));
        meter.add(reply);
        if (!result_ok(reply)) {
          failed = true;
          doom();
          break;
        }
        auto parsed = parse_items(reply.content);
        if (!parsed) {
          failed = true;
          break;
        }
        for (const std::string& item : *parsed) {
          if (candidates.size() >= 500) break;
          if (seen.insert(depot::canonical(item)).second)
            candidates.push_back(item);
        }
      }
    }

    // Local near-duplicate sweep: sim ≥ 0.95 keeps the first occurrence.
    if (!failed && candidates.size() >= 2) {
      std::vector<char> drop(candidates.size(), 0);
      for (std::size_t j = 1; j < candidates.size(); ++j)
        for (std::size_t i = 0; i < j; ++i)
          if (!drop[i] &&
              depot::sim_keys(depot::canonical(candidates[i]),
                              depot::canonical(candidates[j])) >= 0.95) {
            drop[j] = 1;
            break;
          }
      std::vector<std::string> kept;
      kept.reserve(candidates.size());
      for (std::size_t i = 0; i < candidates.size(); ++i)
        if (!drop[i]) kept.push_back(std::move(candidates[i]));
      candidates = std::move(kept);
    }

    // Trims: two rounds to 400 and 300; the light funnel runs one round.
    // A round is skipped when the deduplicated pool is already at or below
    // its target (decision 0024).
    int trim_failures = 0;
    std::string trim_failure_class;
    auto trim_round = [&](int keep) {
      if (failed || candidates.size() <= static_cast<std::size_t>(keep)) return;
      const llm::Result reply = counted(make_request(
          impl_->options, ctx,
          profiled_prompt("trimmer",
                          {{"candidates", join(candidates)},
                           {"keep", std::to_string(keep)}},
                          trimmer_prompt(ctx, candidates, keep)),
          temperature));
      meter.add(reply);
      if (!result_ok(reply)) {
        trim_failures++;
        if (trim_failure_class.empty()) trim_failure_class = last_failure;
        return;
      }
      auto parsed = parse_items(reply.content);
      if (!parsed) {
        trim_failures++;
        return;
      }
      if (!parsed->empty()) candidates = std::move(*parsed);
      if (candidates.size() > static_cast<std::size_t>(keep))
        candidates.resize(keep);
    };
    const std::vector<int>& trims = impl_->options.trim_keeps;
    const int max_trim_failures =
        static_cast<int>(impl_->options.light ? 1 : trims.size());
    if (impl_->options.light) {
      if (!trims.empty()) trim_round(trims.back());
    } else {
      for (const int keep : trims) trim_round(keep);
    }
    if (trim_failures >= max_trim_failures) {
      failed = true;
      if (doomed_by.empty()) doomed_by = trim_failure_class;
    }

    // Critique — only when the campaign turned the critic on (decision
    // 0024). Its score is recorded and its text feeds the synthesizer; it
    // never gates the attempt.
    Critique critique;
    if (!failed && impl_->options.critic) {
      llm::Request critique_request = make_request(
          impl_->options, ctx,
          profiled_prompt("critique", {{"candidates", join(candidates)}},
                          critique_prompt(ctx, candidates)),
          temperature);
      if (!impl_->options.critic_model.empty())
        critique_request.model = impl_->options.critic_model;
      const llm::Result reply = counted(critique_request);
      meter.add(reply);
      if (!result_ok(reply)) {
        failed = true;
        doom();
      } else {
        auto parsed = parse_critique(reply.content);
        if (!parsed) failed = true;
        else critique = std::move(*parsed);
      }
    }

    // Synthesizer.
    std::vector<Child> children;
    if (!failed) {
      std::string critique_text;
      if (impl_->options.critic) {
        std::ostringstream critique_stream;
        critique_stream << "score=" << critique.score << ", remove: ";
        for (std::size_t i = 0; i < critique.remove.size(); ++i)
          critique_stream << (i ? ";" : "") << critique.remove[i];
        critique_stream << ", add: ";
        for (std::size_t i = 0; i < critique.add.size(); ++i)
          critique_stream << (i ? ";" : "") << critique.add[i];
        critique_stream << ", issues: " << critique.issues;
        critique_text = critique_stream.str();
      }
      const llm::Result reply = counted(make_request(
          impl_->options, ctx,
          profiled_prompt(
              "synthesizer",
              {{"candidates", join(candidates)},
               {"critique_issues", critique.issues.empty() ? "none" : critique.issues},
               {"retry_reasons", injected_issues.empty() ? "none" : injected_issues}},
              synthesizer_prompt(ctx, candidates, critique_text,
                                 impl_->options.synth_target_min,
                                 impl_->options.synth_target_max,
                                 impl_->options.synth_max_children,
                                 impl_->options.compact_output, injected_issues)),
          temperature));
      meter.add(reply);
      if (!result_ok(reply)) {
        failed = true;
        doom();
      } else {
        auto parsed = parse_children(reply.content);
        if (!parsed) failed = true;
        else children = std::move(*parsed);
        if (children.size() > 100) children.resize(100);
      }
    }

    // The deterministic checks ARE the gate (decision 0024).
    attempt.children = children;
    attempt.issues = impl_->options.critic ? critique.issues.substr(0, 1000)
                                           : std::string{};
    attempt.score = (!failed && impl_->options.critic) ? critique.score : 0.0;
    if (!failed && !children.empty()) {
      const Checks checks =
          run_checks(children, ctx, impl_->options.synth_max_children,
                     impl_->options.params.children_min,
                     &onion::domain::style_for_child_level(
                         impl_->options.profile, ctx.layer + 1));
      attempt.checks_passed = checks.passed();
      attempt.valid_count = checks.valid_count;
      attempt.child_valid = checks.child_valid;
      attempt.failures = checks.failures;
    } else {
      attempt.checks_passed = false;
      attempt.failures.push_back(failed
                                     ? "a funnel call did not complete or its "
                                       "reply was not valid JSON"
                                     : "the reply contained no children");
    }
    attempt.tokens_in = meter.in_miss;
    attempt.tokens_cached = meter.in_hit;
    attempt.tokens_out = meter.out;
    attempt.cost_usd = meter.cost();

    // A provider failure doomed this attempt: it taught nothing about the
    // node, so it is not an attempt and the node is not judged (F-GATE-2).
    // Completed earlier attempts stay as audit rows.
    if (!doomed_by.empty()) {
      result.decision = Decision::Defer;
      result.end_reason = "provider unavailable";
      result.provider_failure = doomed_by;
      result.attempts = std::move(attempts);
      return result;
    }

    // The failure reasons for the next attempt (capped like critique
    // issues). A passing attempt never needs them.
    injected_issues.clear();
    if (!attempt.checks_passed) {
      for (const std::string& failure : attempt.failures) {
        if (!injected_issues.empty()) injected_issues += "; ";
        injected_issues += failure;
        if (injected_issues.size() > 1000) break;
      }
      if (injected_issues.size() > 1000) injected_issues.resize(1000);
    }

    attempts.push_back(std::move(attempt));
    meters.push_back(meter);

    const Outcome outcome = decide(attempts, impl_->options.params);
    if (outcome.decision == Decision::Retry) continue;

    result.decision = outcome.decision;
    result.end_reason = outcome.end_reason;
    result.chosen = outcome.chosen;
    result.below_target = outcome.below_target;
    if (outcome.decision == Decision::Commit) {
      Attempt& chosen = attempts[outcome.chosen];
      // Commit the valid children only, at most the hard maximum.
      std::vector<Child> kept;
      for (std::size_t i = 0; i < chosen.children.size(); ++i) {
        const bool valid =
            i < chosen.child_valid.size() ? chosen.child_valid[i] != 0 : true;
        if (!valid) continue;
        if (kept.size() >= static_cast<std::size_t>(
                               impl_->options.synth_max_children))
          break;
        kept.push_back(chosen.children[i]);
      }
      chosen.children = std::move(kept);

      // Atomic verification (decision 0021: batched — ONE call) runs on
      // the chosen attempt only (decision 0024). A child the synthesizer
      // labelled atomic is a leaf only when the check names it in
      // admits_none; a failed call keeps every labelled child a task — a
      // provider error must not decide atomicity.
      if (impl_->options.verify_atomic) {
        std::vector<std::string> labelled;
        for (const Child& child : chosen.children)
          if (child.atomic) labelled.push_back(child.title);
        if (!labelled.empty()) {
          // The reply must be able to NAME titles: the budget scales with
          // the labelled list (a 128-token cap truncated the 57-title
          // replies in the DR 0021 measurement).
          const llm::Result verify = counted(make_request(
              impl_->options, ctx,
              profiled_prompt("atomic_verification",
                              {{"labelled", join(labelled)}},
                              atomic_verification_prompt(ctx.title, labelled)),
              0.0,
              std::max(128, 10 * static_cast<int>(labelled.size()))));
          Meter& chosen_meter = meters[outcome.chosen];
          chosen_meter.add(verify);
          chosen.tokens_in = chosen_meter.in_miss;
          chosen.tokens_cached = chosen_meter.in_hit;
          chosen.tokens_out = chosen_meter.out;
          chosen.cost_usd = chosen_meter.cost();
          const auto verdict = result_ok(verify)
                                   ? parse_atomic_verdict(verify.content)
                                   : std::nullopt;
          std::set<std::string> admits_none;
          if (verdict)
            for (const std::string& title : *verdict)
              admits_none.insert(depot::canonical(title));
          for (Child& child : chosen.children)
            if (child.atomic &&
                admits_none.count(depot::canonical(child.title)) == 0)
              child.atomic = false;
        }
      }
    }
    result.attempts = std::move(attempts);
    return result;
  }

  // Unreachable: decide() never returns Retry after max_attempts.
  const Outcome outcome = decide(attempts, impl_->options.params);
  result.decision = outcome.decision;
  result.end_reason = outcome.end_reason;
  result.attempts = std::move(attempts);
  result.chosen = outcome.chosen;
  return result;
}

// ---------------------------------------------------------- quality guard

struct QualityGuard::Impl {
  std::size_t window;
  double min_score;
  double max_failure_rate;
  std::vector<double> scores;
  std::size_t failures{};
  std::size_t total{};
};

QualityGuard::QualityGuard(std::size_t window, double min_score,
                           double max_failure_rate)
    : impl_(std::make_shared<Impl>(
          Impl{window, min_score, max_failure_rate, {}, 0, 0})) {}

void QualityGuard::observe(double score, bool failed) {
  impl_->scores.push_back(score);
  if (impl_->scores.size() > impl_->window)
    impl_->scores.erase(impl_->scores.begin());
  impl_->total++;
  if (failed) impl_->failures++;
}

bool QualityGuard::flagged() const {
  if (impl_->scores.size() < 5) return false;  // not enough evidence
  return rolling_score() < impl_->min_score || failure_rate() > impl_->max_failure_rate;
}

double QualityGuard::rolling_score() const {
  if (impl_->scores.empty()) return 0.0;
  double sum = 0.0;
  for (const double score : impl_->scores) sum += score;
  return sum / static_cast<double>(impl_->scores.size());
}

double QualityGuard::failure_rate() const {
  if (impl_->total == 0) return 0.0;
  return static_cast<double>(impl_->failures) /
         static_cast<double>(impl_->total);
}

void QualityGuard::reset() {
  impl_->scores.clear();
  impl_->failures = 0;
  impl_->total = 0;
}

}  // namespace onion::gate
