#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llm/llm.hpp"

#include "domain/domain.hpp"

namespace onion::gate {
inline constexpr std::string_view component_name{"gate"};

// A child candidate produced by the synthesizer.
struct Child {
  std::string title;
  bool atomic{};
};

// Everything the deterministic checks need about the node being expanded.
struct NodeContext {
  std::string path;
  std::string title;
  int layer{};
  std::vector<std::string> ancestors;  // canonical titles, parent last
  std::vector<std::string> siblings;   // canonical titles
  std::vector<std::string> existing;   // canonical titles of existing children
};

// A finished funnel attempt (audit record content, child-free).
struct Attempt {
  int number{};
  double score{};
  bool checks_passed{};
  std::vector<Child> children;
  std::string issues;  // critique issues, ≤ 1000 characters (decision 0000)
  // Decision 0024: the deterministic checks are the gate.
  std::vector<std::string> failures;  // every failed check, by title
  std::vector<char> child_valid;      // per child: passes repeat/style/similarity
  std::size_t valid_count{0};
  // Metering of the attempt's calls (decision 0003: exact integers/decimals).
  std::uint64_t tokens_in{};
  std::uint64_t tokens_cached{};
  std::uint64_t tokens_out{};
  std::string cost_usd{"0"};
};

// ---- prompts (fixed instruction text first, node text last) ----
std::string system_prompt();
// ≤ 400 tokens: does the node split into at least 3 distinct parts?
std::string precheck_prompt(const NodeContext& ctx);
std::string planner_prompt(const NodeContext& ctx, const std::string& injected_issues,
                           bool light, int aspects_min = 3, int aspects_max = 5);
std::string worker_prompt(const NodeContext& ctx, const std::string& aspect,
                          const std::string& existing_joined,
                          int candidates = 40, bool compact = false);
std::string trimmer_prompt(const NodeContext& ctx,
                           const std::vector<std::string>& candidates, int keep);
std::string critique_prompt(const NodeContext& ctx,
                            const std::vector<std::string>& candidates);
std::string synthesizer_prompt(const NodeContext& ctx,
                               const std::vector<std::string>& candidates,
                               const std::string& critique_text,
                               int target_min = 8, int target_max = 20,
                               int max_children = 30, bool compact = false,
                               const std::string& problems = "");
// The short separate call that confirms a child labelled atomic is a leaf
// (decision 0021: batched — ONE call per parent listing every labelled
// child; the reply names which admit no further decomposition).
std::string atomic_verification_prompt(
    const std::string& node_title,
    const std::vector<std::string>& labelled_children);

// ---- strict parsers (reject malformed bodies) ----
std::optional<std::vector<std::string>> parse_items(const std::string& body);
std::optional<std::vector<Child>> parse_children(const std::string& body);
struct Critique {
  double score{};
  std::vector<std::string> remove;
  std::vector<std::string> add;
  std::string issues;
};
std::optional<Critique> parse_critique(const std::string& body);
std::optional<bool> parse_precheck(const std::string& body);
std::optional<std::vector<std::string>> parse_atomic_verdict(
    const std::string& body);

// ---- deterministic checks (pure) ----
struct Checks {
  bool complete{false};     // finish_reason stop (truncation is a failure)
  bool valid_json{false};
  bool count_ok{false};     // min_children..max_children
  bool no_repeats{false};   // node, ancestors, siblings, existing (canonical)
  bool style_ok{false};
  bool ancestors_ok{false}; // parent ≥ 0.85, higher ancestors ≥ 0.95
  std::vector<std::string> failures;
  std::vector<char> child_valid;  // per child: no repeat/style/similarity fault
  std::size_t valid_count{0};
  bool passed() const;
};
Checks run_checks(const std::vector<Child>& children, const NodeContext& ctx,
                  int max_children = 100, int min_children = 3,
                  const onion::domain::StyleChecks* style = nullptr);

// Defer: the provider failed (outage, rate limit, rejected credentials,
// empty balance). Nothing about the node was learned, so nothing is
// committed — a Leaf would be a permanent tombstone for the subtree.
enum class Decision { Commit, Retry, Leaf, Defer };

// Decision 0024: the gate is the deterministic checks; no score thresholds.
struct GateParams {
  int max_attempts{3};
  int children_min{8};  // the campaign target's lower bound (default 8)
  // The fewest valid children the exhausted-attempts fallback may commit.
  // 3 is decision 0024's fixed floor for the planning default; a domain
  // profile's per-level child range replaces it with the range minimum
  // (decision 0029), so a legitimate 2-step procedure is not leafed.
  int fallback_min{3};
};

struct Outcome {
  Decision decision{Decision::Leaf};
  int chosen{-1};  // index into attempts for Commit
  std::string end_reason;  // children | cannot split
  // Commit by the exhausted-attempts fallback with fewer valid children
  // than children_min.
  bool below_target{false};
};

// Exact decimal-string addition of two non-negative USD amounts (no floats
// in audit records, decision 0003).
std::string add_usd(const std::string& a, const std::string& b);

// Decision 0024, pure: the first attempt whose deterministic checks all pass
// commits; otherwise retry until max_attempts; then the attempt with the most
// valid children (>= 3) commits its valid children, else the node is a leaf.
Outcome decide(const std::vector<Attempt>& attempts, const GateParams& params);

// ---- the attempt loop (pre-check, funnel, decide) ----
// `ask` is a synchronous LLM call; tests inject a fake, production wraps
// llm::Client. Returns the audit attempts (child-free) and the outcome.
struct FunnelResult {
  Decision decision{Decision::Leaf};
  std::string end_reason;
  std::vector<Attempt> attempts;  // audit records, child-free
  int chosen{-1};
  bool precheck_leaf{false};
  bool below_target{false};  // Commit with fewer valid children than the target
  // Decision::Defer only: "transient" (transport, timeout, 5xx, 429),
  // "auth", "balance" or "rejected" (other 4xx).
  std::string provider_failure;
  int calls{};  // LLM completions made (the pre-check + every funnel call)
};

struct FunnelOptions {
  bool light{false};  // campaign's light-funnel depth reached
  // Decision 0024: the critique runs (and is recorded) only when on; it
  // never gates a commit.
  bool critic{false};
  std::string critic_model;  // the critique call's model; empty = `model`
  GateParams params;
  double temperatures[3]{0.0, 0.3, 0.5};
  int max_tokens{6000};
  std::string model{"deepseek-flash"};
  // Funnel configuration (decision 0017, accepted in principle): the
  // defaults ARE the recommended W5 configuration — V1 workers at 40,
  // V2 aspects 3-5, V3 a single trim to 150, the V6 cache prefix on, and
  // the synthesizer targets 8-20 children with a hard maximum of 30 and
  // no padding.  Campaign-spec fields override any of them.
  int worker_candidates{40};         // V1: per-aspect candidates in the worker prompt
  int aspects_min{3};                // V2: planner aspect range (non-light)
  int aspects_max{5};
  std::vector<int> trim_keeps{150};  // V3: full funnel trims in order;
                                     // light runs the last one once
  int synth_max_children{30};        // hard maximum (owner: 30, no padding)
  int synth_target_min{8};           // the range the prompt asks the model to keep
  int synth_target_max{20};
  bool compact_output{false};        // V5: titles array + atomic index list
  std::string cache_prefix;          // V6: shared fixed block prepended to
                                     // every prompt (cache-hit prefix)
  // W5 gate calibration: the DESIGN atomic check (admits_children) runs
  // per labelled-atomic child when on. Default off in M3 — ATOMIC has no
  // store effect yet ("store effects arrive with their milestones"), and
  // the new model labels ~70% of children atomic, which makes the sweep
  // expensive (see DR 0018).
  bool verify_atomic{true};  // decision 0021: batched, on by default
  // Decision 0027: the domain profile that owns the deterministic checks
  // (style, structure layers). Defaults to the embedded planning profile,
  // byte-identical to the historical hard-coded rules.
  onion::domain::Profile profile{onion::domain::planning_profile()};
  // Optional per-1M-token prices (USD per 1M). When set, the attempt cost
  // is computed from usage; when absent, cost stays 0 (node.conf arrives
  // with M5 — decision 0004: no implicit monetary defaults).
  std::optional<onion::llm::PriceSchedule> prices;
};

using SyncAsk = std::function<llm::Result(const llm::Request&)>;

class Funnel {
 public:
  explicit Funnel(FunnelOptions options);
  ~Funnel();
  Funnel(const Funnel&) = delete;
  Funnel& operator=(const Funnel&) = delete;
  Funnel(Funnel&&) noexcept;
  Funnel& operator=(Funnel&&) noexcept;

  FunnelResult run(const NodeContext& ctx, const SyncAsk& ask);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- quality guard (local fixed threshold in M3; fleet median in M6) ----
class QualityGuard {
 public:
  explicit QualityGuard(std::size_t window = 50, double min_score = 0.4,
                        double max_failure_rate = 0.5);
  void observe(double score, bool failed);
  bool flagged() const;   // rolling mean below min_score or failures too high
  double rolling_score() const;
  double failure_rate() const;
  void reset();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace onion::gate
