// depot_core_test.cpp — GoogleTest suite for the data-depot algorithms.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

#include "core/depot_core.hpp"

namespace {

TEST(Trim, StripsWhitespace) {
    EXPECT_EQ(depot::trim("  x  "), "x");
    EXPECT_EQ(depot::trim("a\tb\n"), "a\tb");
    EXPECT_EQ(depot::trim("   "), "");
}

TEST(Lower, Lowercases) {
    EXPECT_EQ(depot::lower("ABC def"), "abc def");
    EXPECT_EQ(depot::lower(""), "");
}

TEST(EndsWith, Basic) {
    EXPECT_TRUE(depot::ends_with("hello", "lo"));
    EXPECT_FALSE(depot::ends_with("hello", "Llo"));
    EXPECT_TRUE(depot::ends_with("hello", "hello"));
    EXPECT_FALSE(depot::ends_with("hi", "hello"));
}

TEST(SplitCSV, PlainFields) {
    auto c = depot::split_csv("a,b,c");
    ASSERT_EQ(c.size(), 3u);
    EXPECT_EQ(c[0], "a");
    EXPECT_EQ(c[1], "b");
    EXPECT_EQ(c[2], "c");
}

TEST(SplitCSV, QuotedFieldWithComma) {
    auto c = depot::split_csv("\"a,b\",c");
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0], "a,b");
    EXPECT_EQ(c[1], "c");
}

TEST(SplitCSV, EscapedQuotes) {
    auto c = depot::split_csv("a,\"b\"\"c\"");
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[1], "b\"c");
}

TEST(SplitCSV, TwoColumns) {
    auto c = depot::split_csv("P01,Identify Problem");
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0], "P01");
    EXPECT_EQ(c[1], "Identify Problem");
}

// Regression: split_pipe("") used to return [""], which wrote empty-title L6 rows.
TEST(SplitPipe, EmptyInputIsEmptyVector) {
    EXPECT_TRUE(depot::split_pipe("").empty());
    EXPECT_TRUE(depot::split_pipe("   ").empty());
}

TEST(SplitPipe, SplitsAndTrims) {
    auto c = depot::split_pipe("a|b|c");
    ASSERT_EQ(c.size(), 3u);
    EXPECT_EQ(c[1], "b");
}

TEST(SplitPipe, SkipsEmptySegments) {
    auto c = depot::split_pipe("a||b|");
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0], "a");
    EXPECT_EQ(c[1], "b");
}

TEST(Sing, PluralRules) {
    EXPECT_EQ(depot::sing("categories"), "category");   // ies -> y
    EXPECT_EQ(depot::sing("boxes"), "box");             // es -> e stripped
    EXPECT_EQ(depot::sing("dogs"), "dog");              // s stripped
    EXPECT_EQ(depot::sing("classes"), "classes");       // -ses kept
    EXPECT_EQ(depot::sing("watches"), "watches");       // -ches kept
    EXPECT_EQ(depot::sing("status"), "status");         // -us kept
    EXPECT_EQ(depot::sing("process"), "process");       // -ss kept
    EXPECT_EQ(depot::sing("analysis"), "analysis");     // -is kept
    EXPECT_EQ(depot::sing("abc"), "abc");               // <=3 chars kept
}

TEST(Canonical, CaseAndPunctuationInvariant) {
    EXPECT_EQ(depot::canonical("E-Commerce"), depot::canonical("E-commerce"));
    EXPECT_EQ(depot::canonical("Follow-Up"), depot::canonical("Follow Up"));
    EXPECT_EQ(depot::canonical("Approved With Conditions"),
              depot::canonical("Approved with conditions"));
}

TEST(Canonical, DropsFillerWords) {
    EXPECT_EQ(depot::canonical("The Order Processing"), depot::canonical("Order Processing"));
    EXPECT_EQ(depot::canonical("Plan of Action"), depot::canonical("Plan Action"));
}

TEST(Canonical, TokenOrderInvariant) {
    EXPECT_EQ(depot::canonical("Customer Data"), depot::canonical("Data Customer"));
}

TEST(Canonical, PluralInvariant) {
    EXPECT_EQ(depot::canonical("Categories"), depot::canonical("Category"));
}

TEST(Canonical, AmpersandAsAnd) {
    EXPECT_EQ(depot::canonical("Risk & Compliance"), depot::canonical("Risk And Compliance"));
}

TEST(LcsRatio, KnownValues) {
    EXPECT_DOUBLE_EQ(depot::lcs_ratio("abc", "abc"), 1.0);
    EXPECT_DOUBLE_EQ(depot::lcs_ratio("abc", "def"), 0.0);
    EXPECT_NEAR(depot::lcs_ratio("abc", "abx"), 2.0 * 2.0 / 6.0, 1e-9);
    EXPECT_DOUBLE_EQ(depot::lcs_ratio("", "abc"), 0.0);
}

TEST(JaccardTokens, KnownValues) {
    EXPECT_DOUBLE_EQ(depot::jaccard_tokens("a b", "a b"), 1.0);
    EXPECT_DOUBLE_EQ(depot::jaccard_tokens("a b", "c d"), 0.0);
    EXPECT_NEAR(depot::jaccard_tokens("a b", "a c"), 1.0 / 3.0, 1e-9);
}

TEST(SimKeys, IdentityAndEmpty) {
    EXPECT_DOUBLE_EQ(depot::sim_keys("a b", "a b"), 1.0);
    EXPECT_DOUBLE_EQ(depot::sim_keys("", ""), 0.0);
    EXPECT_DOUBLE_EQ(depot::sim_keys("x", ""), 0.0);
}

TEST(SimKeys, LexicalTwinsScoreOne) {
    EXPECT_DOUBLE_EQ(depot::sim_keys(depot::canonical("E-Commerce"), depot::canonical("E-commerce")), 1.0);
    EXPECT_DOUBLE_EQ(depot::sim_keys(depot::canonical("Follow Up"), depot::canonical("Follow-Up")), 1.0);
}

TEST(SimKeys, DifferentWordsScoreBelowOne) {
    EXPECT_LT(depot::sim_keys(depot::canonical("Return Merchandise Authorization"),
                              depot::canonical("Supply Chain Risk Rating")), 0.7);
}

TEST(StyleFlags, QuestionMark) {
    auto f = depot::style_flags("hello?");
    EXPECT_NE(std::find(f.begin(), f.end(), "question_or_bang"), f.end());
}

TEST(StyleFlags, NotTitleCase) {
    auto f = depot::style_flags("hello");
    EXPECT_NE(std::find(f.begin(), f.end(), "not_title_case"), f.end());
}

TEST(StyleFlags, TooManyWords) {
    auto f = depot::style_flags("One Two Three Four Five Six Seven Eight Nine");
    EXPECT_NE(std::find(f.begin(), f.end(), "too_many_words"), f.end());
}

TEST(StyleFlags, TrailingPunct) {
    auto f = depot::style_flags("Order Processing.");
    EXPECT_NE(std::find(f.begin(), f.end(), "trailing_punct"), f.end());
}

TEST(StyleFlags, ContainsFiller) {
    auto f = depot::style_flags("Approve with Care");
    EXPECT_NE(std::find(f.begin(), f.end(), "contains_filler"), f.end());
}

TEST(StyleFlags, CleanTitleHasNoFlags) {
    EXPECT_TRUE(depot::style_flags("Order Processing").empty());
}

// Regression: malformed IDs must not crash next_tk (was c_str()+2 UB).
TEST(NextTk, MaxPlusOne) {
    std::vector<depot::Row> l5 = {{"TK0005", "P1-TK0005", "A"}, {"TK0010", "P1-TK0010", "B"}};
    std::vector<depot::Row> l6 = {{"TK0012", "P1-TK0010-TK0012", "C"}};
    EXPECT_EQ(depot::next_tk(l5, l6), 13);
}

TEST(NextTk, MalformedIdsIgnored) {
    std::vector<depot::Row> l5 = {{"X99", "P1-X99", "A"}, {"", "P1", "B"}, {"TK0007", "P1-TK0007", "C"}};
    std::vector<depot::Row> l6 = {{"short", "P1-short", "D"}};
    EXPECT_EQ(depot::next_tk(l5, l6), 8);
}

TEST(NextTk, EmptyDepotStartsAtOne) {
    std::vector<depot::Row> l5, l6;
    EXPECT_EQ(depot::next_tk(l5, l6), 1);
}

TEST(TdPrefix, CutsAtFifthSegment) {
    EXPECT_EQ(depot::td_prefix("P01-IN01-T03-ST47-TD1362-TK15001"),
              "P01-IN01-T03-ST47-TD1362");
}

TEST(JsonEscape, EscapesSpecialChars) {
    EXPECT_EQ(depot::json_escape("a\"b"), "a\\\"b");
    EXPECT_EQ(depot::json_escape("a\\b"), "a\\\\b");
    EXPECT_EQ(depot::json_escape("a\nb"), "a\\nb");
    EXPECT_EQ(depot::json_escape("plain"), "plain");
}

TEST(ParseLlm, ValidAnswer) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"valid\": false, \"fixed\": \"New Title\", \"admits_children\": false}"}}]})";
    auto a = depot::parse_llm(body);
    EXPECT_TRUE(a.parse_ok);
    EXPECT_FALSE(a.valid);
    EXPECT_FALSE(a.admits);
    EXPECT_EQ(a.fixed, "New Title");
}

TEST(ParseLlm, FixedNullMeansNoFix) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"valid\": true, \"fixed\": null, \"admits_children\": true}"}}]})";
    auto a = depot::parse_llm(body);
    EXPECT_TRUE(a.parse_ok);
    EXPECT_TRUE(a.valid);
    EXPECT_TRUE(a.fixed.empty());
}

// Regression: DeepSeek 401 body has no "content" field -> parse error, not crash.
TEST(ParseLlm, AuthErrorBodyReportsParseFailure) {
    const std::string body = R"({"error":{"message":"Authentication Fails, invalid key"}})";
    auto a = depot::parse_llm(body);
    EXPECT_FALSE(a.parse_ok);
    EXPECT_FALSE(a.error.empty());
}

TEST(ParseLlm, TrueFalseInsideFixedStringDoesNotFlipFlags) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"valid\": false, \"fixed\": \"Make it true later\", \"admits_children\": true}"}}]})";
    auto a = depot::parse_llm(body);
    EXPECT_TRUE(a.parse_ok);
    EXPECT_FALSE(a.valid);   // must NOT be flipped by "true" inside fixed
    EXPECT_TRUE(a.admits);
    EXPECT_EQ(a.fixed, "Make it true later");
}

// ---- /orch-fix-defect: strict level parsing ----
TEST(ParseLevels, ValidCommaList) {
    EXPECT_EQ(depot::parse_levels("1,2,3"), (std::vector<int>{1, 2, 3}));
}

TEST(ParseLevels, AcceptsLPrefix) {
    EXPECT_EQ(depot::parse_levels("L5,6"), (std::vector<int>{5, 6}));
}

TEST(ParseLevels, NonNumericIsRejected) {
    EXPECT_TRUE(depot::parse_levels("abc").empty());
    EXPECT_TRUE(depot::parse_levels("1,x,3").empty());
    EXPECT_TRUE(depot::parse_levels("").empty());
}

TEST(ParseLevels, OutOfRangeIsRejected) {
    EXPECT_TRUE(depot::parse_levels("12").empty());
    EXPECT_TRUE(depot::parse_levels("-1").empty());
}

// ---- /orch-fix-defect: TSV field sanitization ----
TEST(SanitizeTsv, ReplacesTabsAndNewlines) {
    EXPECT_EQ(depot::sanitize_tsv("a\tb\nc"), "a b c");
    EXPECT_EQ(depot::sanitize_tsv("clean"), "clean");
}

// ---- critique parser (Evaluation Agent round) ----
TEST(ParseFunnelC, ExtractsScoreRemoveAddIssues) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"score\": 0.75, \"remove\": [\"Bad One\"], \"add\": [\"Missed One\"], \"issues\": \"missing security aspect\"}"}}]})";
    auto c = depot::parse_funnel_c(body);
    EXPECT_NEAR(c.score, 0.75, 1e-9);
    ASSERT_EQ(c.remove.size(), 1u);
    EXPECT_EQ(c.remove[0], "Bad One");
    ASSERT_EQ(c.add.size(), 1u);
    EXPECT_EQ(c.add[0], "Missed One");
    EXPECT_EQ(c.issues, "missing security aspect");
}

TEST(ParseFunnelC, MissingFieldsGetDefaults) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"score\": 0.9}"}}]})";
    auto c = depot::parse_funnel_c(body);
    EXPECT_NEAR(c.score, 0.9, 1e-9);
    EXPECT_TRUE(c.remove.empty());
    EXPECT_TRUE(c.add.empty());
    EXPECT_TRUE(c.issues.empty());
}

TEST(ParseFunnelC, MissingScoreDefaultsToZero) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"remove\": [\"X\"]}"}}]})";
    auto c = depot::parse_funnel_c(body);
    EXPECT_DOUBLE_EQ(c.score, 0.0);  // fail closed: malformed critique retries
    ASSERT_EQ(c.remove.size(), 1u);
}

TEST(ParseFunnelC, MalformedScoreDefaultsToZero) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"score\": \"abc\"}"}}]})";
    auto c = depot::parse_funnel_c(body);
    EXPECT_DOUBLE_EQ(c.score, 0.0);  // unparseable score -> fail closed (retry, don't settle)
}

// ---- real-wire-format regressions (escaped content string) ----
TEST(ParseLlm, RealWireEscapedContent) {
    // exactly what the API returns: content is a JSON string with escaped quotes
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"valid\": false, \"fixed\": \"New Title\", \"admits_children\": false}"}}]})";
    auto a = depot::parse_llm(body);
    EXPECT_TRUE(a.parse_ok);
    EXPECT_FALSE(a.valid);
    EXPECT_FALSE(a.admits);
    EXPECT_EQ(a.fixed, "New Title");
}

TEST(ParseFunnelR1, RealWireEscapedContent) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [\"Alpha\", \"Beta\"]}"}}]})";
    auto items = depot::parse_funnel_r1(body);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1], "Beta");
}

TEST(ParseFunnelC, RealWireEscapedContent) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"score\": 0.5, \"add\": [\"Missed\"], \"issues\": \"gap\"}"}}]})";
    auto c = depot::parse_funnel_c(body);
    EXPECT_NEAR(c.score, 0.5, 1e-9);  // must NOT default to 1.0
    ASSERT_EQ(c.add.size(), 1u);
    EXPECT_EQ(c.issues, "gap");
}

TEST(ParseFunnelR4, PrettyPrintedWireFormatWithNewlines) {
    // v4-pro pretty-prints inside content: \n escapes must decode, not break parsing
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [\n  {\"title\": \"Alpha\", \"atomic\": true},\n  {\"title\": \"Beta\", \"atomic\": false}\n]}"}}]})";
    auto items = depot::parse_funnel_r4(body);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].title, "Alpha");
    EXPECT_TRUE(items[0].atomic);
    EXPECT_EQ(items[1].title, "Beta");
    EXPECT_FALSE(items[1].atomic);
}

TEST(ParseFunnelR1, PrettyPrintedWireFormatWithNewlines) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [\n  \"X\",\n  \"Y\"\n]}"}}]})";
    auto items = depot::parse_funnel_r1(body);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1], "Y");
}

TEST(ParseLlm, PrettyPrintedWireFormatWithNewlines) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\n  \"valid\": false,\n  \"fixed\": \"New\",\n  \"admits_children\": true\n}"}}]})";
    auto a = depot::parse_llm(body);
    EXPECT_TRUE(a.parse_ok);
    EXPECT_FALSE(a.valid);
    EXPECT_EQ(a.fixed, "New");
}

TEST(ParseFunnelR4, SkipsBadObjectKeepsRest) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [{\"atomic\": true}, {\"title\": \"Good\", \"atomic\": true}]}"}}]})";
    auto items = depot::parse_funnel_r4(body);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].title, "Good");
    EXPECT_TRUE(items[0].atomic);
}

// ---- funnel parsers (top 500 -> 400 -> 300 -> best) ----
TEST(ParseFunnelR1, ExtractsItemList) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [\"Alpha\", \"Beta\", \"Gamma\"]}"}}]})";
    auto items = depot::parse_funnel_r1(body);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], "Alpha");
    EXPECT_EQ(items[2], "Gamma");
}

TEST(ParseFunnelR1, HandlesEscapedQuotes) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [\"He said \\\"hi\\\"\"]}"}}]})";
    auto items = depot::parse_funnel_r1(body);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0], "He said \"hi\"");
}

TEST(ParseFunnelR1, MissingItemsKeyIsEmpty) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"other\": [1,2]}"}}]})";
    EXPECT_TRUE(depot::parse_funnel_r1(body).empty());
}

TEST(ParseFunnelR1, EmptyArrayIsEmpty) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    EXPECT_TRUE(depot::parse_funnel_r1(body).empty());
}

TEST(ParseFunnelR4, ExtractsTitleAndAtomic) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Alpha\", \"atomic\": false}, {\"title\": \"Beta\", \"atomic\": true}]}"}}]})";
    auto items = depot::parse_funnel_r4(body);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].title, "Alpha");
    EXPECT_FALSE(items[0].atomic);
    EXPECT_EQ(items[1].title, "Beta");
    EXPECT_TRUE(items[1].atomic);
}

TEST(ParseFunnelR4, KeyOrderDoesNotMatter) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [{\"atomic\": true, \"title\": \"First\"}]}"}}]})";
    auto items = depot::parse_funnel_r4(body);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].title, "First");
    EXPECT_TRUE(items[0].atomic);
}

TEST(ParseFunnelR4, MissingAtomicDefaultsFalse) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Only\"}]}"}}]})";
    auto items = depot::parse_funnel_r4(body);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].title, "Only");
    EXPECT_FALSE(items[0].atomic);
}

TEST(ParseFunnelR4, ObjectWithoutTitleIsEmpty) {
    const std::string body =
        R"({"choices":[{"message":{"content":"{\"items\": [{\"atomic\": true}]}"}}]})";
    EXPECT_TRUE(depot::parse_funnel_r4(body).empty());
}

TEST(ParseLlm, NoBracesInContent) {
    const std::string body = R"({"choices":[{"message":{"content":"just text"}}]})";
    auto a = depot::parse_llm(body);
    EXPECT_FALSE(a.parse_ok);
    EXPECT_FALSE(a.error.empty());
}

// ---------------- thread policy ----------------
TEST(PolicyTarget, IdleMachineGrowsTowardBase) {
    EXPECT_EQ(depot::policy_target(12, 0.1, 8.0, 0, 0, -1, 4), 6);
    EXPECT_EQ(depot::policy_target(12, 0.1, 8.0, 0, 0, -1, 20), 12);  // capped at base
}

TEST(PolicyTarget, BusyMachineShrinks) {
    EXPECT_EQ(depot::policy_target(12, 10.0, 8.0, 0, 0, -1, 8), 6);
    EXPECT_EQ(depot::policy_target(12, 10.0, 8.0, 0, 0, -1, 2), 1);  // floor at 1
}

TEST(PolicyTarget, LowRamHalvesAfterIdleGrowth) {
    EXPECT_EQ(depot::policy_target(12, 0.1, 0.5, 0, 0, -1, 8), 5);  // +2 idle, then /2
}

TEST(PolicyTarget, NicSaturationCapsAtFour) {
    EXPECT_EQ(depot::policy_target(12, 0.1, 8.0, 900, 1000, -1, 12), 4);
}

TEST(PolicyTarget, GpuBusyCapsAtTwo) {
    EXPECT_EQ(depot::policy_target(12, 0.1, 8.0, 0, 0, 95.0, 12), 2);
}

TEST(PolicyTarget, ClampsToRange) {
    EXPECT_EQ(depot::policy_target(1, 0.1, 8.0, 0, 0, -1, 0), 2);   // base min 2
    EXPECT_EQ(depot::policy_target(32, 0.1, 8.0, 0, 0, -1, 64), 16); // base max 16
}

}  // namespace
