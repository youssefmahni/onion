#include "gate.hpp"

#include <algorithm>
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

}  // namespace

std::string system_prompt() {
  return "You are a strict knowledge-curation assistant for a hierarchical "
         "work-breakdown knowledge base that powers AI agents. Do not reason "
         "out loud and do not write any explanation — answer ONLY with the "
         "JSON object requested, nothing else.";
}

std::string precheck_prompt(const NodeContext& ctx) {
  std::ostringstream out;
  out << "This knowledge node must be decomposed into child items for an "
         "AI-agent knowledge base.\n"
      << node_line(ctx) << "\n"
      << "Can this node be split into at least 3 distinct parts, each part a "
         "meaningful direct component of the node itself? Answer ONLY with "
         "JSON {\"splits\": true} or {\"splits\": false}.";
  return out.str();
}

std::string planner_prompt(const NodeContext& ctx,
                           const std::string& injected_issues, bool light) {
  std::ostringstream out;
  if (!injected_issues.empty())
    out << "Previous critique issues you MUST address: " << injected_issues
        << "\n";
  const std::string existing = existing_line(ctx);
  if (!existing.empty()) out << existing << "\n";
  out << "Plan the knowledge coverage for this node: list "
      << (light ? "2-3" : "4-8")
      << " aspects. Every aspect must be a dimension OF THE NODE ITSELF "
         "(things it directly contains or decomposes into) — never related "
         "topics from other domains. Return JSON {\"items\": [ aspects ]}.\n"
      << node_line(ctx);
  return out.str();
}

std::string worker_prompt(const NodeContext& ctx, const std::string& aspect,
                          const std::string& existing_joined) {
  std::ostringstream out;
  out << "Aspect: " << aspect << "\n";
  if (!existing_joined.empty())
    out << "Existing children (do NOT repeat): " << existing_joined << "\n";
  out << "List the top 100 child items of this node for this aspect in an "
         "AI-agent knowledge base. Every item MUST be a direct child of \""
      << ctx.title
      << "\" within its exact scope — do NOT include related topics, industry "
         "classifications, or the node itself. Title Case noun phrases, 2-5 "
         "words, no duplicates, no questions, no emoji. Return JSON "
         "{\"items\": [ ... up to 100 ... ]}.\n"
      << node_line(ctx);
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
         "compliance (Title Case, no questions, no emoji). Candidates:\n"
      << join(candidates)
      << "\nReturn JSON {\"score\": 0.0-1.0 usable-candidate ratio, "
         "\"remove\": [ items to drop ], \"add\": [ missing items to include "
         "], \"issues\": \"short list of problems\"}.\n"
      << node_line(ctx);
  return out.str();
}

std::string synthesizer_prompt(const NodeContext& ctx,
                               const std::vector<std::string>& candidates,
                               const std::string& critique_text) {
  std::ostringstream out;
  out << "From these candidates select the final children set for this node. "
         "Incorporate the critique: drop hallucinated or weak items, include "
         "the missing items you judge valid, resolve any conflicts. You "
         "decide how many to keep (between 3 and 100). For each kept item "
         "return {\"title\": \"...\", \"atomic\": true if it cannot be "
         "decomposed into simpler items, false if it should nest further}. "
         "Candidates:\n"
      << join(candidates) << "\nCritique: " << critique_text
      << "\nReturn JSON {\"items\": [ {\"title\": ..., \"atomic\": ...}, ... "
         "]}.\n"
      << node_line(ctx);
  return out.str();
}

std::string atomic_verification_prompt(const std::string& node_title,
                                       const std::string& child_title) {
  return "The item \"" + child_title + "\" was labelled atomic under \"" +
         node_title +
         "\". Does it admit further decomposition into simpler sub-items for "
         "an AI-agent knowledge base? Answer ONLY with JSON "
         "{\"admits_children\": true} or {\"admits_children\": false}.";
}

std::optional<std::vector<std::string>> parse_items(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object() || !json.contains("items") ||
      !json["items"].is_array())
    return std::nullopt;
  std::vector<std::string> items;
  for (const auto& item : json["items"]) {
    if (!item.is_string()) return std::nullopt;
    items.push_back(item.get<std::string>());
  }
  return items;
}

std::optional<std::vector<Child>> parse_children(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object() || !json.contains("items") ||
      !json["items"].is_array())
    return std::nullopt;
  std::vector<Child> children;
  for (const auto& item : json["items"]) {
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

std::optional<Critique> parse_critique(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object() || !json.contains("score") ||
      !json["score"].is_number())
    return std::nullopt;
  Critique critique;
  critique.score = json["score"].get<double>();
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

std::optional<bool> parse_atomic_verdict(const std::string& body) {
  auto json = nlohmann::json::parse(body, nullptr, false);
  if (json.is_discarded() || !json.is_object() ||
      !json.contains("admits_children") ||
      !json["admits_children"].is_boolean())
    return std::nullopt;
  return json["admits_children"].get<bool>();
}

bool Checks::passed() const {
  return count_ok && no_repeats && style_ok && ancestors_ok;
}

Checks run_checks(const std::vector<Child>& children, const NodeContext& ctx) {
  Checks checks;
  checks.count_ok = children.size() >= 3 && children.size() <= 100;
  if (!checks.count_ok)
    checks.failures.push_back("child count " + std::to_string(children.size()) +
                              " outside 3..100");

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
  const std::string node_canonical = depot::canonical(ctx.title);
  for (const Child& child : children) {
    const std::string canonical = depot::canonical(child.title);
    if (!seen.insert(canonical).second) {
      checks.no_repeats = false;
      checks.failures.push_back("repeated title: " + child.title);
    }
    const auto flags = depot::style_flags(child.title);
    if (!flags.empty()) {
      checks.style_ok = false;
      checks.failures.push_back("style violation: " + child.title);
    }
    // Parent similarity ≥ 0.85 → reject; any higher ancestor ≥ 0.95 → reject.
    if (depot::sim_keys(canonical, node_canonical) >= 0.85) {
      checks.ancestors_ok = false;
      checks.failures.push_back("too similar to parent: " + child.title);
    }
    for (const std::string& ancestor : ctx.ancestors) {
      if (depot::sim_keys(canonical, depot::canonical(ancestor)) >= 0.95) {
        checks.ancestors_ok = false;
        checks.failures.push_back("too similar to ancestor: " + child.title);
        break;
      }
    }
  }
  return checks;
}

Outcome decide(const std::vector<Attempt>& attempts, const GateParams& params) {
  Outcome outcome;
  const auto has_valid_children = [&](const Attempt& attempt) {
    if (!attempt.checks_passed) return false;
    std::set<std::string> distinct;
    for (const Child& child : attempt.children)
      distinct.insert(depot::canonical(child.title));
    return distinct.size() >= 3;
  };

  for (std::size_t i = 0; i < attempts.size(); ++i) {
    const Attempt& attempt = attempts[i];
    if (attempt.checks_passed && attempt.score >= params.pass_score) {
      outcome.decision = Decision::Commit;
      outcome.chosen = static_cast<int>(i);
      outcome.end_reason = "children";
      return outcome;
    }
  }

  if (static_cast<int>(attempts.size()) < params.max_attempts) {
    outcome.decision = Decision::Retry;
    outcome.end_reason = "retry";
    return outcome;
  }

  bool any_valid = false;
  for (const Attempt& attempt : attempts)
    if (has_valid_children(attempt)) any_valid = true;
  if (!any_valid) {
    outcome.decision = Decision::Leaf;
    outcome.end_reason = "cannot split";
    return outcome;
  }

  int best = -1;
  double best_score = -1.0;
  for (std::size_t i = 0; i < attempts.size(); ++i) {
    if (!attempts[i].checks_passed) continue;
    if (attempts[i].score > best_score) {
      best_score = attempts[i].score;
      best = static_cast<int>(i);
    }
  }
  if (best >= 0 && best_score >= params.floor_score) {
    outcome.decision = Decision::Commit;
    outcome.chosen = best;
    outcome.end_reason = "children";
    return outcome;
  }
  outcome.decision = Decision::NeedsReview;
  outcome.chosen = best;  // may be -1
  outcome.end_reason = "below floor";
  return outcome;
}

// ---------------------------------------------------------------- funnel

struct Funnel::Impl {
  FunnelOptions options;
};

Funnel::Funnel(FunnelOptions options)
    : impl_(std::make_unique<Impl>(Impl{options})) {}
Funnel::~Funnel() = default;
Funnel::Funnel(Funnel&&) noexcept = default;
Funnel& Funnel::operator=(Funnel&&) noexcept = default;

namespace {

llm::Request make_request(const FunnelOptions& options, const NodeContext&,
                          std::string user, double temperature,
                          int max_tokens_override = 0) {
  llm::Request request;
  request.model = options.model;
  request.system = system_prompt();
  request.user = std::move(user);
  request.max_tokens = max_tokens_override ? max_tokens_override
                                           : options.max_tokens;
  request.temperature = temperature;
  return request;
}

struct Meter {
  std::uint64_t in_hit{};
  std::uint64_t in_miss{};
  std::uint64_t out{};
  const llm::PriceSchedule* prices{};

  // Exact decimal-string addition (no floats in audit records).
  static std::string add_usd(std::string a, std::string b) {
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
  const SyncAsk counted = [&](const llm::Request& request) -> llm::Result {
    result.calls++;
    return ask(request);
  };

  // Pre-check: at most 400 tokens, temperature 0.
  {
    bool tried = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
      const llm::Result precheck = counted(make_request(
          impl_->options, ctx, precheck_prompt(ctx), 0.0, 400));
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
    (void)tried;  // a failed pre-check runs the funnel; its checks decide
  }

  std::string injected_issues;
  std::vector<Attempt> attempts;
  attempts.reserve(3);

  for (int number = 1; number <= impl_->options.params.max_attempts;
       ++number) {
    Attempt attempt;
    attempt.number = number;
    Meter meter;
    meter.prices = impl_->options.prices ? &*impl_->options.prices : nullptr;
    const double temperature =
        impl_->options.temperatures[(number - 1) % 3];
    bool failed = false;

    // Planner.
    std::vector<std::string> aspects;
    {
      const llm::Result reply = counted(make_request(
          impl_->options, ctx,
          planner_prompt(ctx, injected_issues, impl_->options.light),
          temperature));
      meter.add(reply);
      if (!result_ok(reply)) {
        failed = true;
      } else {
        auto parsed = parse_items(reply.content);
        if (!parsed) failed = true;
        else {
          aspects = std::move(*parsed);
          const std::size_t cap = impl_->options.light ? 3 : 8;
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
            worker_prompt(ctx, aspect, existing_joined), temperature));
        meter.add(reply);
        if (!result_ok(reply)) {
          failed = true;
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
    int trim_failures = 0;
    auto trim_round = [&](int keep) {
      if (failed || candidates.size() <= static_cast<std::size_t>(keep)) return;
      const llm::Result reply = counted(make_request(
          impl_->options, ctx, trimmer_prompt(ctx, candidates, keep),
          temperature));
      meter.add(reply);
      if (!result_ok(reply)) {
        trim_failures++;
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
    if (impl_->options.light) {
      trim_round(300);
    } else {
      trim_round(400);
      trim_round(300);
    }
    if (trim_failures >= (impl_->options.light ? 1 : 2)) failed = true;

    // Critique.
    Critique critique;
    if (!failed) {
      const llm::Result reply = counted(make_request(
          impl_->options, ctx, critique_prompt(ctx, candidates), temperature));
      meter.add(reply);
      if (!result_ok(reply)) {
        failed = true;
      } else {
        auto parsed = parse_critique(reply.content);
        if (!parsed) failed = true;
        else critique = std::move(*parsed);
      }
    }

    attempt.tokens_in = meter.in_miss;
    attempt.tokens_cached = meter.in_hit;
    attempt.tokens_out = meter.out;
    attempt.cost_usd = meter.cost();

    // Synthesizer.
    std::vector<Child> children;
    if (!failed) {
      std::ostringstream critique_text;
      critique_text << "score=" << critique.score << ", remove: ";
      for (std::size_t i = 0; i < critique.remove.size(); ++i)
        critique_text << (i ? ";" : "") << critique.remove[i];
      critique_text << ", add: ";
      for (std::size_t i = 0; i < critique.add.size(); ++i)
        critique_text << (i ? ";" : "") << critique.add[i];
      critique_text << ", issues: " << critique.issues;

      const llm::Result reply = counted(make_request(
          impl_->options, ctx,
          synthesizer_prompt(ctx, candidates, critique_text.str()),
          temperature));
      meter.add(reply);
      if (!result_ok(reply)) {
        failed = true;
      } else {
        auto parsed = parse_children(reply.content);
        if (!parsed) failed = true;
        else children = std::move(*parsed);
        if (children.size() > 100) children.resize(100);
      }
    }

    attempt.children = children;
    attempt.issues = critique.issues.substr(0, 1000);
    if (!failed && !children.empty()) {
      const Checks checks = run_checks(children, ctx);
      attempt.checks_passed = checks.passed();
      attempt.score = critique.score;
    } else {
      attempt.checks_passed = false;
      attempt.score = failed ? 0.0 : critique.score;
    }
    if (failed) attempt.issues = "attempt failed: a funnel call did not complete";

    injected_issues = attempt.issues;
    attempts.push_back(attempt);

    const Outcome outcome = decide(attempts, impl_->options.params);
    if (outcome.decision != Decision::Retry) {
      result.decision = outcome.decision;
      result.end_reason = outcome.end_reason;
      result.attempts = std::move(attempts);
      result.chosen = outcome.chosen;
      return result;
    }
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
