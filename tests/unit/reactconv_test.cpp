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

constexpr const char* kRa1501 = R"(title: RA_1501_get_ability_to_list_processes_executed
id: RA1501
description: Get ability to list processes being executed at the moment.
stage: containment
)";

constexpr const char* kRa1801 = R"(title: RA_1801_collect_memory_dump
id: RA1801
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

TEST(ReactConv, ConvertsToStagesAndActions) {
  const auto prepared =
      parse_action_yaml(kRa1001);
  const auto contained =
      parse_action_yaml(kRa1501);
  const auto recovered =
      parse_action_yaml(kRa1801);
  ASSERT_TRUE(prepared && contained && recovered);
  const auto converted =
      convert({*prepared, *contained, *recovered});

  // Stages in canonical order: preparation, containment, recovery.
  ASSERT_EQ(converted.stages.size(), 3u);
  EXPECT_EQ(converted.stages[0].first, "PREPARATION");
  EXPECT_EQ(converted.stages[1].first, "CONTAINMENT");
  EXPECT_EQ(converted.stages[2].first, "RECOVERY");

  // Actions: code = RA code, path = <STAGE>-<RA code>.
  ASSERT_EQ(converted.actions.size(), 3u);
  EXPECT_EQ(std::get<0>(converted.actions[0]), "RA1001");
  EXPECT_EQ(std::get<1>(converted.actions[0]), "PREPARATION-RA1001");
  EXPECT_EQ(std::get<1>(converted.actions[1]), "CONTAINMENT-RA1501");
  EXPECT_EQ(std::get<1>(converted.actions[2]), "RECOVERY-RA1801");
}

TEST(ReactConv, UnknownStagesSortAfterKnownOnes) {
  Action a{"RA9999", "Odd One", "strange_stage"};
  Action b{"RA1001", "Practice", "preparation"};
  const auto converted = convert({a, b});
  ASSERT_EQ(converted.stages.size(), 2u);
  EXPECT_EQ(converted.stages[0].first, "PREPARATION");
  EXPECT_EQ(converted.stages[1].first, "STRANGE_STAGE");
  EXPECT_EQ(std::get<1>(converted.actions[1]), "STRANGE_STAGE-RA9999");
}
