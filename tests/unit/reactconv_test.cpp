#include <gtest/gtest.h>

#include "tools/reactconv.hpp"

using onion::reactconv::Action;
using onion::reactconv::convert;
using onion::reactconv::parse_action_yaml;

namespace {

constexpr const char* kRa1001 = R"(title: RA_1001_practice
id: RA1001
description: Practice in the real environment.
author: '@atc_project'
creation_date: 2020/04/08
stage: preparation
workflow: |
  Make sure that most of the Response Action has been performed.
automation: |
  You can use PowerBI or other tools.
)";

constexpr const char* kRa3501 = R"(title: RA_3501_get_ability_to_list_processes_executed
id: RA3501
description: Get ability to list processes being executed at the moment.
stage: containment
)";

constexpr const char* kRa5001 = R"(title: RA_5001_collect_memory_dump
id: RA5001
description: Collect a memory dump of a process.
stage: recovery
)";

}  // namespace

TEST(ReactConv, ParsesTopLevelScalarFields) {
  const auto action = parse_action_yaml(kRa1001);
  ASSERT_TRUE(action.has_value());
  EXPECT_EQ(action->id, "RA1001");
  EXPECT_EQ(action->title, "RA_1001_practice");
  EXPECT_EQ(action->stage, "preparation");
}

TEST(ReactConv, IgnoresBlockScalars) {
  // workflow:/automation: block content must not clobber the fields.
  const auto action = parse_action_yaml(kRa1001);
  ASSERT_TRUE(action.has_value());
  EXPECT_EQ(action->stage, "preparation");
  EXPECT_EQ(action->title, "RA_1001_practice");
}

TEST(ReactConv, RejectsDocumentsWithoutIdTitleStage) {
  EXPECT_FALSE(parse_action_yaml("title: only a title\n").has_value());
  EXPECT_FALSE(parse_action_yaml("id: RA1001\nstage: preparation\n")
                   .has_value());
  EXPECT_FALSE(parse_action_yaml("").has_value());
}

TEST(ReactConv, RejectsIdsWhoseStageDoesNotMatchTheYamlStage) {
  EXPECT_FALSE(parse_action_yaml(
      "id: RA1501\ntitle: Wrong stage\nstage: containment\n")
                   .has_value());
  EXPECT_FALSE(parse_action_yaml(
      "id: RA1701\ntitle: Unknown category\nstage: preparation\n")
                   .has_value());
}

TEST(ReactConv, ConvertsToStagesAndActions) {
  const auto prepared =
      parse_action_yaml(kRa1001);
  const auto contained = parse_action_yaml(kRa3501);
  const auto recovered =
      parse_action_yaml(kRa5001);
  ASSERT_TRUE(prepared && contained && recovered);
  const auto converted =
      convert({*prepared, *contained, *recovered});

  // Stages in canonical order: preparation, containment, recovery.
  ASSERT_EQ(converted.stages.size(), 3u);
  EXPECT_EQ(converted.stages[0].first, "S1");
  EXPECT_EQ(converted.stages[1].first, "S3");
  EXPECT_EQ(converted.stages[2].first, "S5");

  // Categories are materialized between stages and actions. Their C<digit>
  // codes are scoped by their parent stage in the path.
  ASSERT_EQ(converted.categories.size(), 3u);
  EXPECT_EQ(std::get<0>(converted.categories[0]), "C0");
  EXPECT_EQ(std::get<1>(converted.categories[0]), "S1-C0");
  EXPECT_EQ(std::get<1>(converted.categories[1]), "S3-C5");

  // Actions: code = RA code, path = S<stage>-C<category>-<RA code>.
  ASSERT_EQ(converted.actions.size(), 3u);
  EXPECT_EQ(std::get<0>(converted.actions[0]), "RA1001");
  EXPECT_EQ(std::get<1>(converted.actions[0]), "S1-C0-RA1001");
  EXPECT_EQ(std::get<1>(converted.actions[1]), "S3-C5-RA3501");
  EXPECT_EQ(std::get<1>(converted.actions[2]), "S5-C0-RA5001");
}

TEST(ReactConv, TitlesComeFromTheBundleAndFallBackToAReadableSlug) {
  using onion::reactconv::humanize_title;
  using onion::reactconv::parse_action_names;
  EXPECT_EQ(humanize_title("RA_3101_block_external_ip_address"),
            "Block external ip address");
  EXPECT_EQ(humanize_title("Practice"), "Practice");  // not a slug
  EXPECT_EQ(humanize_title("RA_31_short"), "RA_31_short");

  const auto names = parse_action_names(R"({
    "type": "bundle",
    "objects": [
      {"type": "x-react-action", "name": "Block external IP address",
       "external_references": [{"source_name": "atc-react",
                                "external_id": "RA3101"}]},
      {"type": "x-react-stage", "name": "Containment",
       "external_references": [{"external_id": "RS0003"}]},
      {"type": "x-react-action", "name": "Broken entry"}
    ]})");
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names.at("RA3101"), "Block external IP address");
  EXPECT_TRUE(parse_action_names("not json").empty());

  const Action action{"RA3101", "RA_3101_block_external_ip_address",
                      "containment"};
  EXPECT_EQ(std::get<2>(convert({action}, names).actions[0]),
            "Block external IP address");
  EXPECT_EQ(std::get<2>(convert({action}).actions[0]),
            "Block external ip address");
}

TEST(ReactConv, IgnoresInvalidDirectActions) {
  Action a{"RA9999", "Odd One", "strange_stage"};
  Action b{"RA1001", "Practice", "preparation"};
  const auto converted = convert({a, b});
  ASSERT_EQ(converted.stages.size(), 1u);
  EXPECT_EQ(converted.stages[0].first, "S1");
  ASSERT_EQ(converted.actions.size(), 1u);
  EXPECT_EQ(std::get<1>(converted.actions[0]), "S1-C0-RA1001");
}
