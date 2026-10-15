#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "domain/domain.hpp"
#include "gate/gate.hpp"

namespace {

using nlohmann::json;

json react_document() {
  const std::filesystem::path path =
      std::filesystem::path(ONION_SOURCE_DIR) / "docs/react.domain-profile.json";
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input) << path;
  return json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

onion::domain::Profile react_profile() {
  std::string error;
  const auto profile = onion::domain::parse_profile(react_document(), &error);
  EXPECT_TRUE(profile) << error;
  return profile ? *profile : onion::domain::Profile{};
}

TEST(DomainProfile, PlanningCanonicalFormRemainsTheLegacyDefault) {
  const auto& planning = onion::domain::planning_profile();
  const json expected = {
      {"name", "planning"},
      {"tree", ""},
      {"structure_layers", {0, 1, 2}},
      {"style", {{"title_case", true},
                 {"max_words", 8},
                 {"no_questions", true},
                 {"no_trailing_punct", true},
                 {"filler_words", {"the", "a", "an", "of", "and", "or",
                                   "for", "to", "in", "on", "at", "with",
                                   "by", "from", "vs", "versus", "into",
                                   "within"}}}},
  };
  EXPECT_EQ(onion::domain::profile_json(planning), expected);
  std::string error;
  const auto parsed = onion::domain::parse_profile(expected, &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(onion::domain::profile_hash(*parsed),
            onion::domain::profile_hash(planning));
}

TEST(DomainProfile, LoadsTheCompleteReactProfileAndBindsItsContract) {
  const json document = react_document();
  std::string error;
  const auto parsed = onion::domain::parse_profile(document, &error);
  ASSERT_TRUE(parsed) << error;
  const auto& profile = *parsed;
  EXPECT_EQ(profile.tree, "react");
  EXPECT_EQ(profile.structure_layers, (std::vector<int>{0, 1}));
  EXPECT_EQ(profile.light_funnel_from, 5);
  const auto environment_range =
      onion::domain::child_range_for_parent(profile, 3);
  ASSERT_TRUE(environment_range);
  EXPECT_EQ(environment_range->min, 3);
  EXPECT_EQ(environment_range->max, 10);
  EXPECT_TRUE(onion::domain::ordered_children_for_parent(profile, 3));
  EXPECT_FALSE(onion::domain::ordered_children_for_parent(profile, 2));
  EXPECT_EQ(onion::domain::level_name(profile, 4), "Step");
  EXPECT_NE(onion::domain::child_level_guidance(profile, 3).find("ordered"),
            std::string::npos);
  EXPECT_EQ(onion::domain::profile_json(profile), document);
  EXPECT_NE(onion::domain::profile_hash(profile),
            onion::domain::profile_hash(onion::domain::planning_profile()));
}

TEST(DomainProfile, EnforcesReactTitleRulesAndRejectsFloatingAnchorScores) {
  const auto profile = react_profile();
  const auto& step_style = onion::domain::style_for_child_level(profile, 4);
  EXPECT_TRUE(onion::domain::style_flags("apply rule?", step_style).size() > 0);
  const auto& operation_style =
      onion::domain::style_for_child_level(profile, 5);
  EXPECT_TRUE(onion::domain::style_flags("Run `iptables -A INPUT`",
                                         operation_style)
                  .empty());
  EXPECT_NE(onion::domain::title_rules(step_style).find("imperative"),
            std::string::npos);

  json invalid = react_document();
  invalid["critique_anchors"][0]["score"] = 0.8;
  std::string error;
  EXPECT_FALSE(onion::domain::parse_profile(invalid, &error));
  EXPECT_NE(error.find("exact decimal text"), std::string::npos);
}

TEST(DomainProfile, RendersTitleRulesAndKeepsParserContractInCode) {
  onion::gate::FunnelOptions options;
  options.profile = react_profile();
  options.params.children_min = 3;
  options.synth_target_min = 3;
  options.synth_target_max = 10;
  options.synth_max_children = 10;
  options.verify_atomic = false;
  std::string captured_system;
  std::string captured_worker;
  onion::gate::Funnel funnel(options);
  const onion::gate::NodeContext context{
      "S3-C1-RA3101", "Block External IP Address", 2,
      {"Containment", "Network"}, {}, {}};
  const auto reply = [&](const onion::llm::Request& request) {
    captured_system = request.system;
    if (request.user.find("Can this node be split") != std::string::npos)
      return onion::llm::Result{onion::llm::Kind::Ok, 200,
                                R"({"splits":true})"};
    if (request.user.find("List 3 to 5 aspects") != std::string::npos)
      return onion::llm::Result{onion::llm::Kind::Ok, 200,
                                R"({"items":["Platforms"]})"};
    if (request.user.find("List up to 40 candidate children") !=
        std::string::npos) {
      captured_worker = request.user;
      return onion::llm::Result{onion::llm::Kind::Ok, 200,
                                R"({"items":["Windows Defender Firewall","AWS Security Groups","Linux nftables"]})"};
    }
    if (request.user.find("Choose the final children") != std::string::npos)
      return onion::llm::Result{onion::llm::Kind::Ok, 200,
                                R"({"items":[{"title":"Windows Defender Firewall","atomic":false},{"title":"AWS Security Groups","atomic":false},{"title":"Linux nftables","atomic":false}]})"};
    return onion::llm::Result{onion::llm::Kind::Malformed, 0, {}};
  };
  const auto result = funnel.run(context, reply);
  EXPECT_EQ(result.decision, onion::gate::Decision::Commit);
  EXPECT_NE(captured_system.find("incident-response knowledge curator"),
            std::string::npos);
  EXPECT_NE(captured_worker.find("noun phrase naming the real product"),
            std::string::npos);
  EXPECT_NE(captured_worker.find("Return JSON {\"items\""),
            std::string::npos);
}

// ---- load-time validation ------------------------------------------------

// Mutates the react document and expects the loader to refuse it with a
// message containing `expected`.
void ExpectRejected(const std::function<void(json&)>& mutate,
                    const std::string& expected) {
  json document = react_document();
  mutate(document);
  std::string error;
  EXPECT_FALSE(onion::domain::parse_profile(document, &error))
      << "accepted a profile that should fail with: " << expected;
  EXPECT_NE(error.find(expected), std::string::npos)
      << "error was: " << error;
}

TEST(DomainProfileValidation, RejectsUnknownAndMalformedFields) {
  ExpectRejected([](json& d) { d["surprise"] = 1; }, "unknown field surprise");
  ExpectRejected([](json& d) { d["levels"]["max"] = "9"; }, "levels.max");
  ExpectRejected(
      [](json& d) { d["style_levels"][0]["min_words"] = 9; },
      "min_words exceeds max_words");
  ExpectRejected(
      [](json& d) { d["levels"]["definitions"][3]["children"]["max"] = 500; },
      "range must be 1..100");
  ExpectRejected(
      [](json& d) {
        d["description"] = std::string(300 * 1024, 'x');
      },
      "larger than 256 KiB");
}

TEST(DomainProfileValidation, PromptsMustBeCompleteAndUseOnlyFilledPlaceholders) {
  ExpectRejected([](json& d) { d["prompts"].erase("trimmer"); },
                 "prompts.trimmer is required");
  ExpectRejected(
      [](json& d) {
        d["prompts"]["worker"] =
            d["prompts"]["worker"].get<std::string>() + " {made_up}";
      },
      "uses {made_up}");
  // {keep} exists, but the planner is never given it.
  ExpectRejected(
      [](json& d) {
        d["prompts"]["planner"] =
            d["prompts"]["planner"].get<std::string>() + " {keep}";
      },
      "prompts.planner uses {keep}");
  // A worker prompt without {aspect} would send the same call per aspect.
  ExpectRejected(
      [](json& d) {
        std::string text = d["prompts"]["worker"].get<std::string>();
        for (std::size_t at; (at = text.find("{aspect}")) != std::string::npos;)
          text.replace(at, 8, "the aspect");
        d["prompts"]["worker"] = text;
      },
      "must contain {aspect}");
  ExpectRejected([](json& d) { d["placeholders"].push_back("bogus"); },
                 "which the engine never fills");
  // style_levels is only honest if the prompts render the rules.
  ExpectRejected(
      [](json& d) {
        std::string text = d["prompts"]["synthesizer"].get<std::string>();
        for (std::size_t at; (at = text.find("{title_rules}")) != std::string::npos;)
          text.replace(at, 13, "");
        d["prompts"]["synthesizer"] = text;
      },
      "must render {title_rules}");
}

TEST(DomainProfileValidation, LevelsRangesAndStylesMustBeCoherent) {
  // Level 0's children (categories) are imported, so it has no range.
  ExpectRejected(
      [](json& d) {
        d["levels"]["definitions"][0]["children"] = {{"min", 3}, {"max", 5}};
      },
      "children are imported");
  ExpectRejected(
      [](json& d) { d["levels"]["definitions"][2].erase("source"); },
      "source disagrees with structure.imported_layers");
  ExpectRejected(
      [](json& d) { d["levels"]["definitions"][1]["source"] = "generated"; },
      "source must be \"import\" or absent");
  // A title rule for a level the engine never generates is a mistake.
  ExpectRejected(
      [](json& d) { d["style_levels"][0]["levels"] = json::array({2}); },
      "imported level 2");
  // Definitions past levels.max are refused outright ...
  ExpectRejected([](json& d) { d["levels"]["max"] = 4; },
                 "within levels.max");
  // ... and rules that name a level past it are refused by the coherence
  // check (the draft styles levels 5..9, so max 6 leaves 7..9 dangling).
  ExpectRejected([](json& d) { d["levels"]["max"] = 6; },
                 "outside 1..levels.max");
  ExpectRejected(
      [](json& d) { d["atomic"]["expected_from_level"] = 9; d["levels"]["max"] = 6; },
      "levels.max");
}

TEST(DomainProfileValidation, DeeperLevelsRepeatTheLastDefinitionUpToMax) {
  const auto profile = react_profile();
  // The draft ends its list at "Sub-operation" (level 6) with max 9.
  EXPECT_EQ(onion::domain::level_name(profile, 6), "Sub-operation");
  EXPECT_EQ(onion::domain::level_name(profile, 8), "Sub-operation");
  const auto deep = onion::domain::child_range_for_parent(profile, 8);
  ASSERT_TRUE(deep);
  EXPECT_EQ(deep->min, 2);
  EXPECT_EQ(deep->max, 4);
  EXPECT_FALSE(onion::domain::child_range_for_parent(profile, 10));
  EXPECT_EQ(onion::domain::level_name(profile, 10), "L10");
  // The planning profile declares no levels at all.
  EXPECT_FALSE(onion::domain::child_range_for_parent(
      onion::domain::planning_profile(), 3));
  EXPECT_EQ(onion::domain::planning_profile().max_level, -1);
}

// ---- style_levels are enforced in code and rendered for the model --------

TEST(DomainProfileStyle, EnvironmentTitlesAreProductNounPhrases) {
  const auto profile = react_profile();
  const auto& style = onion::domain::style_for_child_level(profile, 3);
  const auto flags = [&](const std::string& title) {
    return onion::domain::style_flags(title, style);
  };
  EXPECT_TRUE(flags("Windows Defender Firewall").empty());
  EXPECT_TRUE(flags("Palo Alto Networks NGFW").empty());
  EXPECT_TRUE(flags("iOS Mobile Device Management").empty());  // product caps
  EXPECT_FALSE(flags("Block the IP quickly?").empty());
  EXPECT_FALSE(flags("Cloud.").empty());                        // trailing period
  EXPECT_FALSE(flags("A B C D E F G").empty());                 // > 6 words
  EXPECT_FALSE(flags(std::string(61, 'A')).empty());            // > 60 chars
  EXPECT_FALSE(flags("Firewall \xF0\x9F\x94\xA5").empty());     // emoji
  EXPECT_FALSE(flags("firewall").empty());                      // no capital
  EXPECT_FALSE(flags("Firewall; Proxy").empty());               // punctuation
  const std::string rules = onion::domain::title_rules(style);
  for (const char* part : {"noun phrase naming the real product", "1-6 words",
                           "at most 60 characters", "no emoji",
                           "no trailing period", "only punctuation"})
    EXPECT_NE(rules.find(part), std::string::npos) << part << " in " << rules;
}

TEST(DomainProfileStyle, StepAndOperationRulesDifferByLevel) {
  const auto profile = react_profile();
  const auto& step = onion::domain::style_for_child_level(profile, 4);
  const auto& operation = onion::domain::style_for_child_level(profile, 5);
  const auto& deepest = onion::domain::style_for_child_level(profile, 9);
  EXPECT_TRUE(onion::domain::style_flags("Preserve volatile evidence", step)
                  .empty());
  EXPECT_FALSE(onion::domain::style_flags("Preserve", step).empty());  // 1 word
  // A code span is a level-5+ allowance: a step may not carry one ...
  EXPECT_FALSE(onion::domain::style_flags("Run `ls -l` now", step).empty());
  // ... an operation may, but only when the backticks pair up.
  EXPECT_TRUE(onion::domain::style_flags(
                  "Run `iptables -A INPUT -s <IP> -j DROP`", operation)
                  .empty());
  EXPECT_FALSE(
      onion::domain::style_flags("Run `iptables -A INPUT", operation).empty());
  // The profile allows a backslash inside an operation (Windows paths).
  EXPECT_TRUE(onion::domain::style_flags(
                  "Open `C:\\Windows\\System32\\winevt\\Logs`", operation)
                  .empty());
  EXPECT_FALSE(onion::domain::style_flags("Run it now?", operation).empty());
  // Levels 5..9 share one rule set.
  EXPECT_TRUE(onion::domain::style_flags("Query `Get-Service`", deepest)
                  .empty());
  // A level with no style_levels entry falls back to the (empty) default.
  EXPECT_TRUE(onion::domain::style_flags(
                  "anything goes?!", onion::domain::style_for_child_level(
                                         profile, 1))
                  .empty());
}

TEST(DomainProfileStyle, PlanningStyleIsUntouchedByTheNewFields) {
  const auto& planning = onion::domain::planning_profile();
  EXPECT_EQ(onion::domain::style_for_child_level(planning, 4).max_words, 8);
  EXPECT_TRUE(onion::domain::style_flags("Order Processing Rules",
                                         planning.style)
                  .empty());
  const auto flags =
      onion::domain::style_flags("the order of processing steps", planning.style);
  EXPECT_FALSE(flags.empty());
}

TEST(DomainProfileStyle, PlanningTitlesMayContainBackticks) {
  // A regression guard: the code-span rule belongs to profiles that set it.
  // The planning style never does, so a backticked planning title is valid.
  const auto& planning = onion::domain::planning_profile();
  EXPECT_TRUE(onion::domain::style_flags("Configure `nginx` Server",
                                         planning.style)
                  .empty());
  onion::domain::StyleChecks strict;
  strict.allow_code_span = false;
  EXPECT_FALSE(onion::domain::style_flags("Configure `nginx` Server", strict)
                   .empty());
  strict.allow_code_span = true;
  EXPECT_TRUE(onion::domain::style_flags("Run `nginx -t`", strict).empty());
  EXPECT_FALSE(onion::domain::style_flags("Run `nginx -t", strict).empty());
}

TEST(DomainProfileStyle, EveryEnforcedRuleIsRenderedAndCharactersAreCounted) {
  onion::domain::StyleChecks style;
  style.filler_words = {"the", "of"};
  style.no_trailing_punct = true;
  style.allow_code_span = false;
  style.max_chars = 5;
  const std::string rules = onion::domain::title_rules(style);
  for (const char* part : {"the, of", "no trailing period or colon",
                           "no code spans", "at most 5 characters"})
    EXPECT_NE(rules.find(part), std::string::npos) << part << " in " << rules;
  // max_chars counts characters: five 2-byte letters fit, six do not.
  EXPECT_TRUE(onion::domain::style_flags("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9",
                                         style)
                  .empty());
  EXPECT_FALSE(onion::domain::style_flags(
                   "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", style)
                   .empty());
}

TEST(DomainProfileValidation, RefusesNearMissPlaceholderSpellings) {
  for (const char* spelling : {"{Candidates}", "{TITLE}", "{ title }", "{made-up}"})
    ExpectRejected(
        [&](json& d) {
          d["prompts"]["worker"] =
              d["prompts"]["worker"].get<std::string>() + " " + spelling;
        },
        "which that stage is never given");
}

TEST(DomainProfileValidation, CoherenceChecksDoNotDependOnHavingDefinitions) {
  std::string error;
  // levels.max is declared but definitions is empty: a layer past max is
  // still refused.
  EXPECT_FALSE(onion::domain::parse_profile(
      json{{"name", "x"},
           {"levels", {{"max", 2}, {"definitions", json::array()}}},
           {"structure_layers", json::array({7})}},
      &error));
  EXPECT_NE(error.find("beyond levels.max"), std::string::npos) << error;
  // A definition that says "import" with no imported_layers is refused.
  ExpectRejected(
      [](json& d) { d["structure"].erase("imported_layers"); },
      "source disagrees with structure.imported_layers");
  // "planning" is the export filter name for the planning tree.
  ExpectRejected([](json& d) { d["tree"] = "planning"; }, "reserved");
}

TEST(DomainProfileValidation, SizeCapAppliesToThePublishedForm) {
  // profile_json() adds tree/structure_layers/style before signing, so a
  // document sized exactly to the cap grows past it and must be refused at
  // publish time, not at fold time on every device.
  const auto document_with_padding = [](std::size_t padding) {
    return json{{"name", "x"}, {"description", std::string(padding, 'a')}};
  };
  const std::size_t base = document_with_padding(0).dump().size();
  const std::size_t cap = onion::domain::detail::kMaxProfileBytes;
  std::string error;
  EXPECT_FALSE(onion::domain::parse_profile(document_with_padding(cap - base),
                                            &error));
  EXPECT_NE(error.find("larger than 256 KiB"), std::string::npos) << error;
  // Comfortably inside the cap, and re-parsing the signed form agrees.
  const auto accepted =
      onion::domain::parse_profile(document_with_padding(cap - base - 200),
                                   &error);
  ASSERT_TRUE(accepted) << error;
  const auto reparsed = onion::domain::parse_profile(
      onion::domain::profile_json(*accepted), &error);
  ASSERT_TRUE(reparsed) << error;
  EXPECT_EQ(onion::domain::profile_hash(*reparsed),
            onion::domain::profile_hash(*accepted));
}

// ---- prompt rendering ----------------------------------------------------

TEST(DomainProfileRendering, SubstitutionIsSinglePass) {
  onion::gate::FunnelOptions options;
  options.profile = react_profile();
  options.params.children_min = 3;
  options.synth_target_min = 3;
  options.synth_target_max = 10;
  options.synth_max_children = 10;
  options.verify_atomic = false;
  std::string captured_worker;
  onion::gate::Funnel funnel(options);
  // A hostile title that itself looks like a placeholder must stay literal.
  const onion::gate::NodeContext context{
      "S3-C1-RA3101", "Block {siblings} {title_rules} IP", 2,
      {"Containment", "Network"}, {"SIBLINGMARK"}, {}};
  const auto reply = [&](const onion::llm::Request& request) {
    if (request.user.find("Can this node be split") != std::string::npos)
      return onion::llm::Result{onion::llm::Kind::Ok, 200,
                                R"({"splits":true})"};
    if (request.user.find("aspects") != std::string::npos &&
        request.user.find("candidate children") == std::string::npos)
      return onion::llm::Result{onion::llm::Kind::Ok, 200,
                                R"({"items":["Platforms"]})"};
    if (request.user.find("candidate children") != std::string::npos) {
      captured_worker = request.user;
      return onion::llm::Result{onion::llm::Kind::Ok, 200,
                                R"({"items":["Windows Defender Firewall"]})"};
    }
    return onion::llm::Result{onion::llm::Kind::Malformed, 0, {}};
  };
  funnel.run(context, reply);
  ASSERT_FALSE(captured_worker.empty());
  EXPECT_NE(captured_worker.find("Block {siblings} {title_rules} IP"),
            std::string::npos)
      << captured_worker;
  EXPECT_NE(captured_worker.find("SIBLINGMARK"), std::string::npos);
}

}  // namespace
