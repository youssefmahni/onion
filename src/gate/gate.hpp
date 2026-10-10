#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "llm/llm.hpp"

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
                           bool light);
std::string worker_prompt(const NodeContext& ctx, const std::string& aspect,
                          const std::string& existing_joined);
std::string trimmer_prompt(const NodeContext& ctx,
                           const std::vector<std::string>& candidates, int keep);
std::string critique_prompt(const NodeContext& ctx,
                            const std::vector<std::string>& candidates);
std::string synthesizer_prompt(const NodeContext& ctx,
                               const std::vector<std::string>& candidates,
                               const std::string& critique_text);
// The short separate call that confirms a child labelled atomic is a leaf.
std::string atomic_verification_prompt(const std::string& node_title,
                                       const std::string& child_title);

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
std::optional<bool> parse_atomic_verdict(const std::string& body);

// ---- deterministic checks (pure) ----
struct Checks {
  bool complete{false};     // finish_reason stop (truncation is a failure)
  bool valid_json{false};
  bool count_ok{false};     // 3..100 children
  bool no_repeats{false};   // node, ancestors, siblings, existing (canonical)
  bool style_ok{false};
  bool ancestors_ok{false}; // parent ≥ 0.85, higher ancestors ≥ 0.95
  std::vector<std::string> failures;
  bool passed() const;
};
Checks run_checks(const std::vector<Child>& children, const NodeContext& ctx);

enum class Decision { Commit, Retry, Leaf, NeedsReview };

struct GateParams {
  double pass_score{0.7};
  double floor_score{0.5};
  int max_attempts{3};
};

struct Outcome {
  Decision decision{Decision::Leaf};
  int chosen{-1};  // index into attempts for Commit / NeedsReview
  std::string end_reason;  // children | cannot split | below floor
};

// Best-of-3 per DESIGN.md › Quality gate and retries, pure.
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
  int calls{};  // LLM completions made (the pre-check + every funnel call)
};

struct FunnelOptions {
  bool light{false};  // campaign's light-funnel depth reached
  GateParams params;
  double temperatures[3]{0.0, 0.3, 0.5};
  int max_tokens{6000};
  std::string model{"deepseek-flash"};
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
