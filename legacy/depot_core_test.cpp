// depot_core_test.cpp — GoogleTest suite for the data-depot algorithms.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "depot_core.hpp"
#include "depot_io.hpp"

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

// ---------------- integration: run_ingest on a hermetic mini-depot ----------------
class IngestFixture : public ::testing::Test {
protected:
    std::filesystem::path dir_;

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("depot_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir_);
        write_file("Onion - level_04.csv",
                   "ID,ID-L4,Title\n"
                   "TD1,P01-IN01-T01-ST01-TD1,Map current processes\n"
                   "TD2,P01-IN01-T02-ST02-TD2,Document risk register\n");
        write_file("Onion - level_05.csv",
                   "ID,ID-L5,Title\n"
                   "TK0001,P01-IN01-T01-ST01-TD1-TK0001,Order Processing\n"
                   "TK0002,P01-IN01-T02-ST02-TD2-TK0002,Register ID\n");
        write_file("Onion - level_06.csv", "ID,ID-L6,Title\n");
        write_file("expand_decisions.csv",
                   "Title,Instances,Decision,SubItems,Source\n"
                   "ERP,1,expand,Finance Module|HR Module,SUBS\n");
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    void write_file(const std::string& name, const std::string& content) {
        std::ofstream f(dir_ / name);
        f << content;
    }

    std::vector<depot::Row> load_file(const std::string& name) {
        std::vector<depot::Row> out;
        std::ifstream f(dir_ / name);
        std::string line;
        std::getline(f, line);  // header
        while (std::getline(f, line)) {
            auto c = depot::split_csv(line);
            if (c.size() >= 3)
                out.push_back({depot::trim(c[0]), depot::trim(c[1]), depot::trim(c[2])});
        }
        return out;
    }
};

TEST_F(IngestFixture, VerdictsOnMiniDepot) {
    auto tsv = dir_ / "research.tsv";
    {
        std::ofstream f(tsv);
        f << "Map current processes\tOrder Processing\n"          // exact dup -> skip
          << "Map current processes\tE-Commerce\n"                // new -> added
          << "Map current processes\tOrder Processings\n"         // plural twin -> skip
          << "Document risk register\tERP\n"                      // new + decision subs
          << "Bogus TD\tWhatever\n";                              // no matching TD
    }
    testing::internal::CaptureStdout();
    int rc = depot::run_ingest(tsv.string(), false, "P01-IN01-", dir_.string());
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);

    auto l5 = load_file("Onion - level_05.csv");
    auto l6 = load_file("Onion - level_06.csv");
    ASSERT_GE(l5.size(), 4u);   // 2 originals + E-Commerce + ERP
    EXPECT_EQ(l6.size(), 2u);   // ERP's two decision subs
    bool has_ecommerce = false, has_erp = false;
    for (const auto& r : l5) {
        if (r.title == "E-Commerce") has_ecommerce = true;
        if (r.title == "ERP") has_erp = true;
    }
    EXPECT_TRUE(has_ecommerce);
    EXPECT_TRUE(has_erp);
    // gates: no dup ids, no orphans
    std::set<std::string> ids, paths;
    for (const auto& r : l5) { ids.insert(r.id); paths.insert(r.path); }
    for (const auto& r : l6) {
        ids.insert(r.id);
        EXPECT_TRUE(paths.count(r.path.substr(0, r.path.rfind('-'))));
    }
}

TEST_F(IngestFixture, LexicallySimilarIsAutoMerged) {
    write_file("Onion - level_05.csv",
               "ID,ID-L5,Title\n"
               "TK0001,P01-IN01-T01-ST01-TD1-TK0001,Order Processing\n"
               "TK0002,P01-IN01-T02-ST02-TD2-TK0002,Register ID\n"
               "TK0003,P01-IN01-T01-ST01-TD1-TK0003,Return Merchandise Authorization\n");
    auto tsv = dir_ / "research.tsv";
    {
        std::ofstream f(tsv);
        f << "Map current processes\tReturn Merchandise Authorisation\n";  // LCS ~0.96
    }
    testing::internal::CaptureStdout();
    depot::run_ingest(tsv.string(), false, "P01-IN01-", dir_.string());
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("merged=1"), std::string::npos);
    auto l5 = load_file("Onion - level_05.csv");
    EXPECT_EQ(l5.size(), 3u);  // merged, nothing added
    std::ifstream al(dir_ / "title_aliases.tsv");
    std::string line;
    std::getline(al, line);
    EXPECT_NE(line.find("auto-merge"), std::string::npos);
}

TEST_F(IngestFixture, CandidateBandIsAddedNotMerged) {
    write_file("Onion - level_05.csv",
               "ID,ID-L5,Title\n"
               "TK0001,P01-IN01-T01-ST01-TD1-TK0001,Order Processing\n"
               "TK0002,P01-IN01-T02-ST02-TD2-TK0002,Register ID\n"
               "TK0003,P01-IN01-T01-ST01-TD1-TK0003,Return Merchandise Authorization\n");
    auto tsv = dir_ / "research.tsv";
    {
        std::ofstream f(tsv);
        f << "Map current processes\tReturn Merchandise Handling\n";  // ~0.73 -> candidate
    }
    testing::internal::CaptureStdout();
    depot::run_ingest(tsv.string(), false, "P01-IN01-", dir_.string());
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("candidate=1"), std::string::npos);
    auto l5 = load_file("Onion - level_05.csv");
    EXPECT_EQ(l5.size(), 4u);
}

TEST_F(IngestFixture, SpellingNormalizedToDepotForm) {
    write_file("Onion - level_05.csv",
               "ID,ID-L5,Title\n"
               "TK0001,P01-IN01-T01-ST01-TD1-TK0001,Order Processing\n"
               "TK0002,P01-IN01-T02-ST02-TD2-TK0002,Register ID\n"
               "TK0003,P01-IN01-T01-ST01-TD1-TK0003,E-Commerce\n");
    auto tsv = dir_ / "research.tsv";
    {
        std::ofstream f(tsv);
        f << "Document risk register\tE-commerce\n";  // lexical twin in another TD
    }
    testing::internal::CaptureStdout();
    depot::run_ingest(tsv.string(), false, "P01-IN01-", dir_.string());
    testing::internal::GetCapturedStdout();
    auto l5 = load_file("Onion - level_05.csv");
    bool depot_spelling = false, research_spelling = false;
    for (const auto& r : l5) {
        if (r.title == "E-Commerce") depot_spelling = true;
        if (r.title == "E-commerce") research_spelling = true;
    }
    EXPECT_TRUE(depot_spelling);
    EXPECT_FALSE(research_spelling);
    std::ifstream al(dir_ / "title_aliases.tsv");
    std::string line;
    std::getline(al, line);
    EXPECT_NE(line.find("spelling-normalized"), std::string::npos);
}

TEST_F(IngestFixture, DryRunLeavesFilesUntouched) {
    auto before = load_file("Onion - level_05.csv");
    auto tsv = dir_ / "research.tsv";
    {
        std::ofstream f(tsv);
        f << "Map current processes\tBrand New Item\n";
    }
    testing::internal::CaptureStdout();
    depot::run_ingest(tsv.string(), true, "P01-IN01-", dir_.string());
    testing::internal::GetCapturedStdout();
    auto after = load_file("Onion - level_05.csv");
    EXPECT_EQ(before.size(), after.size());
}

TEST_F(IngestFixture, StyleFlaggedButStillAdded) {
    auto tsv = dir_ / "research.tsv";
    {
        std::ofstream f(tsv);
        f << "Map current processes\tbad style?\n";
    }
    testing::internal::CaptureStdout();
    depot::run_ingest(tsv.string(), false, "P01-IN01-", dir_.string());
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("flagged=1"), std::string::npos);
    auto l5 = load_file("Onion - level_05.csv");
    bool added = false;
    for (const auto& r : l5)
        if (r.title == "bad style?") added = true;
    EXPECT_TRUE(added);
}

TEST_F(IngestFixture, MissingInputFileFails) {
    EXPECT_EQ(depot::run_ingest((dir_ / "nope.tsv").string(), false, "", dir_.string()), 1);
}

// ---------------- integration: funnel with mocked LLM transport ----------------
class FunnelFixture : public ::testing::Test {
protected:
    std::filesystem::path dir_;

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("depot_funnel_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir_);
        write_file("Onion - level_00.csv", "ID,Phase\nP01,Identify Problem or Opportunity\n");
        write_file("Onion - level_01.csv", "ID,ID-L1,DOC\nIN01,P01-IN01,Identify Problem or Opportunity\n");
        write_file("Onion - level_02.csv", "ID,ID-L2,Tasks & Activities\nT01,P01-IN01-T01,Business Discovery\n");
        write_file("Onion - level_03.csv", "ID,ID-L3,Title\nST01,P01-IN01-T01-ST01,Business Context\n");
        write_file("Onion - level_04.csv", "ID,ID-L4,Title\nTD1,P01-IN01-T01-ST01-TD1,Define business domain\n");
        write_file("Onion - level_05.csv",
                   "ID,ID-L5,Title\nTK0001,P01-IN01-T01-ST01-TD1-TK0001,Existing Item\n");
        for (int l = 6; l <= 9; ++l)
            write_file("Onion - level_0" + std::to_string(l) + ".csv",
                       "ID,ID-L" + std::to_string(l) + ",Title\n");
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    void write_file(const std::string& name, const std::string& content) {
        std::ofstream f(dir_ / name);
        f << content;
    }

    std::vector<depot::Row> load_file(const std::string& name) {
        std::vector<depot::Row> out;
        std::ifstream f(dir_ / name);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            auto c = depot::split_csv(line);
            if (c.size() >= 3)
                out.push_back({depot::trim(c[0]), depot::trim(c[1]), depot::trim(c[2])});
        }
        return out;
    }
};

TEST_F(FunnelFixture, ApiFailureIsRetriedNotMarkedDone) {
    std::atomic<int> plan_calls{0};
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        if (payload.find("Plan the knowledge coverage") != std::string::npos) {
            if (plan_calls.fetch_add(1) == 0)
                return R"({"error":{"message":"upstream 500"}})";  // transport failure
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Solo\"]}"}}]})";
        }
        if (payload.find("Aspect:") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Alpha\"]}"}}]})";
        if (payload.find("Critique this candidate set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"score\": 0.9, \"remove\": [], \"add\": [], \"issues\": \"\"}"}}]})";
        if (payload.find("select the final children set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Alpha\", \"atomic\": true}]}"}}]})";
        return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 1, false, false, "deepseek-chat",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("errors=1"), std::string::npos);    // first attempt failed
    EXPECT_NE(out.find("funneled=2"), std::string::npos);  // node retried and completed
    auto l5 = load_file("Onion - level_05.csv");
    ASSERT_EQ(l5.size(), 2u);  // Existing + Alpha
    std::ifstream st(dir_ / "funnel_state.tsv");
    std::string line;
    int state_rows = 0;
    while (std::getline(st, line)) state_rows++;
    EXPECT_EQ(state_rows, 1);  // only the successful pass is recorded
}

TEST_F(FunnelFixture, ForceReRunsSettledSubtree) {
    // force must re-queue the settled node AND walk into its children, so a
    // whole settled subtree is re-expanded (not just the seed).
    write_file("funnel_state.tsv",
               "P01-IN01-T01-ST01-TD1\t0\t0\t0\t1.0\t1\n");
    depot::AskFn fake = [](const std::string& payload) -> std::string {
        if (payload.find("Plan the knowledge coverage") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Solo\"]}"}}]})";
        if (payload.find("Aspect:") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
        if (payload.find("Critique this candidate set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"score\": 0.9, \"remove\": [], \"add\": [], \"issues\": \"\"}"}}]})";
        if (payload.find("select the final children set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
        return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 1, false, true, "deepseek-chat",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("funneled=2"), std::string::npos);   // TD1 + TK0001 both re-run
    EXPECT_NE(out.find("NODE_START\tP01-IN01-T01-ST01-TD1-TK0001"), std::string::npos);
}

TEST_F(FunnelFixture, NodeCannotGenerateItself) {
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        if (payload.find("Plan the knowledge coverage") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Solo\"]}"}}]})";
        if (payload.find("Aspect:") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Define business domain\", \"Fresh Item\"]}"}}]})";
        if (payload.find("Critique this candidate set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"score\": 0.9, \"remove\": [], \"add\": [], \"issues\": \"\"}"}}]})";
        if (payload.find("select the final children set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Define business domain\", \"atomic\": true}, {\"title\": \"Fresh Item\", \"atomic\": true}]}"}}]})";
        return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 1, false, true, "deepseek-chat",
                               "sk-fake", dir_.string(), fake);
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    auto l5 = load_file("Onion - level_05.csv");
    bool self = false, fresh = false;
    for (const auto& r : l5) {
        if (r.title == "Define business domain") self = true;
        if (r.title == "Fresh Item") fresh = true;
    }
    EXPECT_FALSE(self);  // the node's own title was filtered
    EXPECT_TRUE(fresh);
}

TEST_F(FunnelFixture, RoundsDoNotRerunSettledLowScoreNodes) {
    std::atomic<int> td_plans{0};
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        bool is_td = payload.find("Title: Define business domain") != std::string::npos;
        bool retry = payload.find("Previous critique issues") != std::string::npos;
        if (payload.find("Plan the knowledge coverage") != std::string::npos) {
            if (is_td) td_plans++;
            return retry
                ? R"({"choices":[{"message":{"content":"{\"items\": [\"Ops\"]}"}}]})"
                : R"({"choices":[{"message":{"content":"{\"items\": [\"Ops\"]}"}}]})";
        }
        if (payload.find("Aspect:") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Alpha\"]}"}}]})";
        if (payload.find("Critique this candidate set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"score\": 0.5, \"remove\": [], \"add\": [], \"issues\": \"weak\"}"}}]})";
        if (payload.find("select the final children set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Alpha\", \"atomic\": true}]}"}}]})";
        return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    };
    write_file("Onion - level_05.csv", "ID,ID-L5,Title\n");  // no pre-existing children
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 2, false, false, "deepseek-v4-pro",
                               "sk-fake", dir_.string(), fake, "", 2 /*rounds*/);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    // the settled TD must NOT be re-run by round 2: exactly 2 planner calls for it
    // (first pass + its one retry), never 4 (one per round)
    EXPECT_EQ(td_plans.load(), 2);
}

TEST_F(FunnelFixture, DryRunWritesNothing) {
    depot::AskFn fake = [&](const std::string&) {
        return R"({"choices":[{"message":{"content":"{\"items\": [\"Only\"]}"}}]})";
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 5, 1, true, true, "deepseek-chat",
                               "sk-fake", dir_.string(), fake);
    testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    auto l5 = load_file("Onion - level_05.csv");
    EXPECT_EQ(l5.size(), 1u);  // untouched
    EXPECT_FALSE(std::filesystem::exists(dir_ / "funnel_state.tsv"));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "funnel_critique.tsv"));
}

TEST_F(FunnelFixture, MockedFunnelNestsToAtomicLeaves) {
    // four-agent rounds, routed by prompt content (thread-safe, order-independent)
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        bool is_td = payload.find("Title: Define business domain") != std::string::npos;
        if (payload.find("Plan the knowledge coverage") != std::string::npos)
            return is_td
                ? R"({"choices":[{"message":{"content":"{\"items\": [\"Operations\", \"Governance\"]}"}}]})"
                : R"({"choices":[{"message":{"content":"{\"items\": [\"Sub\"]}"}}]})";
        if (payload.find("Aspect:") != std::string::npos) {
            if (payload.find("Aspect: Operations") != std::string::npos)
                return R"({"choices":[{"message":{"content":"{\"items\": [\"Alpha\", \"Gamma\"]}"}}]})";
            if (payload.find("Aspect: Governance") != std::string::npos)
                return R"({"choices":[{"message":{"content":"{\"items\": [\"Delta\", \"Epsilon\"]}"}}]})";
            return R"({"choices":[{"message":{"content":"{\"items\": [\"X\", \"Y\", \"Z\"]}"}}]})";
        }
        if (payload.find("Critique this candidate set") != std::string::npos) {
            if (is_td)  // TD critique: add the missed item -> synthesizer fixes the miss
                return R"({"choices":[{"message":{"content":"{\"score\": 0.9, \"remove\": [], \"add\": [\"Beta\"], \"issues\": \"\"}"}}]})";
            return R"({"choices":[{"message":{"content":"{\"score\": 0.9, \"remove\": [], \"add\": [], \"issues\": \"\"}"}}]})";
        }
        if (payload.find("select the final children set") != std::string::npos) {
            if (is_td)
                return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Alpha\", \"atomic\": false}, {\"title\": \"Beta\", \"atomic\": true}, {\"title\": \"Existing Item\", \"atomic\": true}]}"}}]})";
            return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"X\", \"atomic\": true}]}"}}]})";
        }
        return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 4, false, false, "deepseek-chat",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("funneled=2"), std::string::npos);  // TD1 + Alpha
    // TD: P + 2W + C + S = 5; Alpha: P + 1W + C + S = 4
    EXPECT_NE(out.find("calls=9"), std::string::npos);

    auto l5 = load_file("Onion - level_05.csv");
    auto l6 = load_file("Onion - level_06.csv");
    ASSERT_EQ(l5.size(), 3u);  // Existing + Alpha + Beta (critique's "add" fixed the miss)
    bool has_alpha = false, has_beta = false, dup = false;
    for (const auto& r : l5) {
        if (r.title == "Alpha") has_alpha = true;
        if (r.title == "Beta") has_beta = true;
        if (r.title == "Existing Item" && r.id != "TK0001") dup = true;
    }
    EXPECT_TRUE(has_alpha);
    EXPECT_TRUE(has_beta);
    EXPECT_FALSE(dup);
    ASSERT_EQ(l6.size(), 1u);  // Alpha's atomic child X
    EXPECT_EQ(l6[0].title, "X");
    std::ifstream st(dir_ / "funnel_state.tsv");
    std::string line;
    int state_rows = 0;
    while (std::getline(st, line)) state_rows++;
    EXPECT_EQ(state_rows, 2);
    std::ifstream cr(dir_ / "funnel_critique.tsv");
    int crit_rows = 0;
    while (std::getline(cr, line)) crit_rows++;
    EXPECT_EQ(crit_rows, 2);
}

TEST_F(FunnelFixture, LowScoreNodeIsRetriedAndFixed) {
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        bool is_td = payload.find("Title: Define business domain") != std::string::npos;
        bool retry = payload.find("Previous critique issues") != std::string::npos;
        if (payload.find("Plan the knowledge coverage") != std::string::npos)
            return retry
                ? R"({"choices":[{"message":{"content":"{\"items\": [\"Ops\", \"Gov\"]}"}}]})"
                : R"({"choices":[{"message":{"content":"{\"items\": [\"Ops\"]}"}}]})";
        if (payload.find("Aspect:") != std::string::npos) {
            if (payload.find("Aspect: Gov") != std::string::npos)
                return R"({"choices":[{"message":{"content":"{\"items\": [\"Beta\"]}"}}]})";
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Alpha\"]}"}}]})";
        }
        bool has_beta = payload.find("Beta") != std::string::npos;
        if (payload.find("Critique this candidate set") != std::string::npos) {
            if (is_td && !has_beta)
                return R"({"choices":[{"message":{"content":"{\"score\": 0.5, \"remove\": [], \"add\": [], \"issues\": \"missed governance\"}"}}]})";
            return R"({"choices":[{"message":{"content":"{\"score\": 0.9, \"remove\": [], \"add\": [], \"issues\": \"\"}"}}]})";
        }
        if (payload.find("select the final children set") != std::string::npos) {
            if (is_td && !has_beta)
                return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Alpha\", \"atomic\": true}]}"}}]})";
            if (is_td && has_beta)
                return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Beta\", \"atomic\": true}]}"}}]})";
            return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"X\", \"atomic\": true}]}"}}]})";
        }
        return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 2, false, false, "deepseek-chat",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("retried=1"), std::string::npos);   // the miss was re-funneled
    EXPECT_NE(out.find("funneled=2"), std::string::npos);  // TD1 twice, no children (all atomic)
    auto l5 = load_file("Onion - level_05.csv");
    // P7: the failed first attempt commits nothing, so only Existing + Beta.
    // (This test used to expect Alpha too, which encoded the bug.)
    ASSERT_EQ(l5.size(), 2u);
    for (const auto& r : l5) EXPECT_NE(r.title, "Alpha");
    // only the final (score>=0.7) pass is recorded in state
    std::ifstream st(dir_ / "funnel_state.tsv");
    std::string line;
    int state_rows = 0;
    while (std::getline(st, line)) state_rows++;
    EXPECT_EQ(state_rows, 1);
}

// ---------------- live-resizable pool ----------------
TEST_F(FunnelFixture, PoolResizesFromControlFile) {
    // Deterministic synchronization: the synthesizer reply holds the funnel
    // open until the manager has demonstrably announced "THREADS\t3" on the
    // run's stdout.  A reader thread watches a pipe copy of stdout and
    // releases the synthesizer only after it sees the announcement.  This
    // removes the wall-clock race that made the test flake on loaded CI
    // runners (the manager could be starved for the whole funnel window).
    std::atomic<bool> raised{false};
    std::mutex synth_mutex;
    std::condition_variable synth_cv;
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        // Slow-ish replies keep the funnel itself well beyond one control
        // interval, as in the original timing-based test.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (payload.find("Plan the knowledge coverage") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [\"A1\", \"A2\"]}"}}]})";
        if (payload.find("Aspect:") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"items\": [\"Alpha\"]}"}}]})";
        if (payload.find("Critique this candidate set") != std::string::npos)
            return R"({"choices":[{"message":{"content":"{\"score\": 0.9, \"remove\": [], \"add\": [], \"issues\": \"\"}"}}]})";
        if (payload.find("select the final children set") != std::string::npos) {
            // Bounded wait: if the manager never raises, the run still ends
            // and the assertion below reports the real failure (no hang).
            // The bound is deliberately long: the legacy control loop uses
            // cv.wait_for with a clock that can jump on hosted runners
            // (observed ~10 s stalls on macOS), so a short bound can expire
            // just before a stalled manager wakes and reconciles.
            std::unique_lock<std::mutex> lock(synth_mutex);
            synth_cv.wait_for(lock, std::chrono::seconds(120),
                              [&] { return raised.load(); });
            return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Alpha\", \"atomic\": true}]}"}}]})";
        }
        return R"({"choices":[{"message":{"content":"{\"items\": []}"}}]})";
    };
    // control dir + initial target 1 -> then raise to 3 mid-run
    std::filesystem::create_directories(dir_ / "control");
    {
        std::ofstream f(dir_ / "control/concurrency.txt");
        f << "3\n";  // raise 1 -> 3 workers
    }
    setenv("DEPOT_CONTROL_INTERVAL_MS", "50", 1);
    // Route the run's stdout through a pipe so the reader can watch for the
    // manager's announcement without racing the test's own thread.
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);
    std::cout << std::flush;
    ::fflush(stdout);
    const int saved_stdout = ::dup(1);
    ASSERT_GE(saved_stdout, 0);
    ASSERT_EQ(::dup2(pipefd[1], 1), 1);

    std::string observed;
    std::atomic<bool> run_done{false};
    std::thread reader([&] {
        char buffer[4096];
        while (!run_done.load()) {
            const ssize_t n = ::read(pipefd[0], buffer, sizeof(buffer));
            if (n > 0) {
                observed.append(buffer, static_cast<std::size_t>(n));
                if (!raised.load() &&
                    observed.find("THREADS\t3") != std::string::npos) {
                    raised.store(true);
                    synth_cv.notify_all();
                }
            } else if (n == 0) {
                break;  // every writer closed
            }
        }
        for (;;) {  // drain anything left until EOF
            const ssize_t n = ::read(pipefd[0], buffer, sizeof(buffer));
            if (n <= 0) break;
            observed.append(buffer, static_cast<std::size_t>(n));
        }
    });

    const int rc = depot::run_funnel("P01-IN01-", 10, 1, false, true,
                                     "deepseek-v4-pro", "sk-fake", dir_.string(),
                                     fake);

    std::cout << std::flush;
    ::fflush(stdout);
    run_done.store(true);
    ::close(pipefd[1]);
    ::dup2(saved_stdout, 1);
    ::close(saved_stdout);
    reader.join();
    ::close(pipefd[0]);
    unsetenv("DEPOT_CONTROL_INTERVAL_MS");
    EXPECT_EQ(rc, 0);
    EXPECT_NE(observed.find("THREADS\t3"), std::string::npos)
        << "manager never raised the pool; captured run output:\n" << observed;
}

// ---------------- integration: run_llm traversal (hermetic, no network) ----------------
class LlmTraversalFixture : public ::testing::Test {
protected:
    std::filesystem::path dir_;

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("depot_llm_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir_);
        write_file("Onion - level_00.csv", "ID,Phase\nP01,Identify Problem or Opportunity\nP02,Define Business Case\n");
        write_file("Onion - level_01.csv",
                   "ID,ID-L1,DOC\nIN01,P01-IN01,Identify Problem or Opportunity\n");
        write_file("Onion - level_02.csv",
                   "ID,ID-L2,Tasks & Activities\nT01,P01-IN01-T01,Business Discovery\n");
        write_file("Onion - level_03.csv",
                   "ID,ID-L3,Title\nST01,P01-IN01-T01-ST01,Business Context\n");
        write_file("Onion - level_04.csv",
                   "ID,ID-L4,Title\nTD1,P01-IN01-T01-ST01-TD1,Define business domain\n");
        write_file("Onion - level_05.csv",
                   "ID,ID-L5,Title\nTK0001,P01-IN01-T01-ST01-TD1-TK0001,Cybersecurity\n");
        write_file("Onion - level_06.csv", "ID,ID-L6,Title\n");
        write_file("Onion - level_07.csv", "ID,ID-L7,Title\n");
        write_file("Onion - level_08.csv", "ID,ID-L8,Title\n");
        write_file("Onion - level_09.csv", "ID,ID-L9,Title\n");
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    void write_file(const std::string& name, const std::string& content) {
        std::ofstream f(dir_ / name);
        f << content;
    }
};

// offset past the end -> empty work queue -> traversal machinery runs with zero API calls
TEST_F(LlmTraversalFixture, EmptyQueueCompletesWithoutNetwork) {
    testing::internal::CaptureStdout();
    int rc = depot::run_llm({0,1,2,3,4,5,6,7,8,9}, 0, 999, 2, true, true,
                            "deepseek-chat", "sk-fake", dir_.string());
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("nodes to check: 0"), std::string::npos);
    EXPECT_NE(out.find("done 0"), std::string::npos);
}

// non-dry run with empty queue still writes a header-only report
TEST_F(LlmTraversalFixture, WritesHeaderOnlyReport) {
    testing::internal::CaptureStdout();
    depot::run_llm({5}, 0, 999, 2, false, true, "deepseek-chat", "sk-fake", dir_.string());
    testing::internal::GetCapturedStdout();
    std::ifstream f(dir_ / "node_llm_report.tsv");
    std::string line;
    std::getline(f, line);
    EXPECT_EQ(line, "path\tlayer\ttitle\tparent\tchildren\tvalid\tfixed\tadmits_children\terror");
}

}  // namespace

// ---------------- M0 stabilization regressions ----------------
static std::string ok_body(const std::string& inner_escaped) {
    return "{\"choices\":[{\"message\":{\"content\":\"" + inner_escaped +
           "\"},\"finish_reason\":\"stop\"}]}";
}

TEST(ReplyTruncated, DetectsLengthFinishReason) {  // P4
    EXPECT_TRUE(depot::reply_truncated(
        R"({"choices":[{"message":{"content":"{\"items\": [\"A\""},"finish_reason":"length"}]})"));
    EXPECT_TRUE(depot::reply_truncated(
        "{\"choices\":[{\"finish_reason\" : \"length\"}]}"));
    EXPECT_FALSE(depot::reply_truncated(
        R"({"choices":[{"message":{"content":"{}"},"finish_reason":"stop"}]})"));
    EXPECT_FALSE(depot::reply_truncated(R"({"choices":[{"message":{"content":"{}"}}]})"));
    // the words inside escaped message content are not the envelope's field
    EXPECT_FALSE(depot::reply_truncated(
        R"({"choices":[{"message":{"content":"{\"finish_reason\":\"length\"}"},"finish_reason":"stop"}]})"));
}

TEST_F(FunnelFixture, TruncatedReplyIsAFailedAttempt) {  // P4
    std::atomic<int> synth_calls{0};
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        if (payload.find("Plan the knowledge coverage") != std::string::npos)
            return ok_body("{\\\"items\\\": [\\\"Solo\\\"]}");
        if (payload.find("Aspect:") != std::string::npos)
            return ok_body("{\\\"items\\\": [\\\"Alpha\\\", \\\"Beta\\\"]}");
        if (payload.find("Critique this candidate set") != std::string::npos)
            return ok_body("{\\\"score\\\": 0.9, \\\"remove\\\": [], \\\"add\\\": [], \\\"issues\\\": \\\"\\\"}");
        if (payload.find("select the final children set") != std::string::npos) {
            if (synth_calls.fetch_add(1) == 0)  // first reply cut off mid-list
                return R"({"choices":[{"message":{"content":"{\"items\": [{\"title\": \"Alpha\", \"atomic\": true}"},"finish_reason":"length"}]})";
            return ok_body("{\\\"items\\\": [{\\\"title\\\": \\\"Alpha\\\", \\\"atomic\\\": true}, {\\\"title\\\": \\\"Beta\\\", \\\"atomic\\\": true}]}");
        }
        return ok_body("{\\\"items\\\": []}");
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 1, false, false, "deepseek-flash",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("reply cut off at max_tokens"), std::string::npos);
    auto l5 = load_file("Onion - level_05.csv");
    EXPECT_EQ(l5.size(), 3u);  // Existing + Alpha + Beta, only from the complete reply
}

TEST_F(FunnelFixture, RetryCommitsOnlyTheBetterAttempt) {  // P7
    depot::AskFn fake = [&](const std::string& payload) -> std::string {
        bool retry = payload.find("Previous critique issues") != std::string::npos;
        bool is_td = payload.find("Title: Define business domain") != std::string::npos;
        if (payload.find("Plan the knowledge coverage") != std::string::npos)
            return ok_body("{\\\"items\\\": [\\\"Ops\\\"]}");
        if (payload.find("Aspect:") != std::string::npos)
            return retry ? ok_body("{\\\"items\\\": [\\\"Beta\\\"]}")
                         : ok_body("{\\\"items\\\": [\\\"Alpha\\\"]}");
        bool has_beta = payload.find("Beta") != std::string::npos;
        if (payload.find("Critique this candidate set") != std::string::npos) {
            if (!is_td) return ok_body("{\\\"score\\\": 0.9, \\\"remove\\\": [], \\\"add\\\": [], \\\"issues\\\": \\\"\\\"}");
            // first attempt 0.6, the retry is WORSE at 0.4
            return has_beta ? ok_body("{\\\"score\\\": 0.4, \\\"remove\\\": [], \\\"add\\\": [], \\\"issues\\\": \\\"worse\\\"}")
                            : ok_body("{\\\"score\\\": 0.6, \\\"remove\\\": [], \\\"add\\\": [], \\\"issues\\\": \\\"weak\\\"}");
        }
        if (payload.find("select the final children set") != std::string::npos)
            return has_beta ? ok_body("{\\\"items\\\": [{\\\"title\\\": \\\"Beta\\\", \\\"atomic\\\": true}]}")
                            : ok_body("{\\\"items\\\": [{\\\"title\\\": \\\"Alpha\\\", \\\"atomic\\\": true}]}");
        return ok_body("{\\\"items\\\": []}");
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 1, false, false, "deepseek-flash",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(out.find("retried=1"), std::string::npos);
    auto l5 = load_file("Onion - level_05.csv");
    bool alpha = false, beta = false;
    for (const auto& r : l5) {
        if (r.title == "Alpha") alpha = true;
        if (r.title == "Beta") beta = true;
    }
    EXPECT_TRUE(alpha);   // the better (0.6) attempt is committed
    EXPECT_FALSE(beta);   // the worse retry is discarded
    std::ifstream st(dir_ / "funnel_state.tsv");
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(st, line)));
    EXPECT_NE(line.find("\t0.6\t"), std::string::npos);  // settled with the kept score
    EXPECT_FALSE(static_cast<bool>(std::getline(st, line)));  // exactly one row
    std::ifstream cr(dir_ / "funnel_critique.tsv");
    int crit_rows = 0;
    while (std::getline(cr, line)) crit_rows++;
    EXPECT_EQ(crit_rows, 1);  // the discarded attempt left no critique row
}

TEST_F(FunnelFixture, TransportFailuresNeverSettleNodes) {  // P6
    depot::AskFn fake = [](const std::string&) { return std::string(); };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 1, false, false, "deepseek-flash",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);  // 2 failures stay below the default breaker limit of 20
    EXPECT_NE(out.find("NODE_GIVEUP\tP01-IN01-T01-ST01-TD1"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "funnel_state.tsv"));  // nothing settled at 0.0
}

TEST_F(FunnelFixture, CircuitBreakerStopsTheRun) {  // P6
    setenv("DEPOT_BREAKER_FAILS", "2", 1);
    depot::AskFn fake = [](const std::string&) {
        return std::string(R"({"error":{"message":"invalid api key"}})");
    };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 10, 1, false, false, "deepseek-flash",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    unsetenv("DEPOT_BREAKER_FAILS");
    EXPECT_EQ(rc, 3);
    EXPECT_NE(out.find("BREAKER_OPEN"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir_ / "funnel_state.tsv"));
}

TEST_F(FunnelFixture, DuplicatePathsAreReported) {  // P3
    write_file("Onion - level_05.csv",
               "ID,ID-L5,Title\n"
               "TK0001,P01-IN01-T01-ST01-TD1-TK0001,Existing Item\n"
               "TK0001,P01-IN01-T01-ST01-TD1-TK0001,Conflicting Title\n");
    depot::AskFn fake = [](const std::string&) { return ok_body("{\\\"items\\\": []}"); };
    testing::internal::CaptureStdout();
    int rc = depot::run_funnel("P01-IN01-", 1, 1, false, false, "deepseek-flash",
                               "sk-fake", dir_.string(), fake);
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_EQ(rc, 0);  // default: report and keep running, as today
    EXPECT_NE(out.find("DUP_PATHS\t1"), std::string::npos);
    std::ifstream df(dir_ / "duplicate_paths.tsv");
    std::string header, row;
    ASSERT_TRUE(static_cast<bool>(std::getline(df, header)));
    ASSERT_TRUE(static_cast<bool>(std::getline(df, row)));
    EXPECT_NE(row.find("Existing Item"), std::string::npos);
    EXPECT_NE(row.find("Conflicting Title"), std::string::npos);

    setenv("DEPOT_STRICT_PATHS", "1", 1);
    testing::internal::CaptureStdout();
    rc = depot::run_funnel("P01-IN01-", 1, 1, false, false, "deepseek-flash",
                           "sk-fake", dir_.string(), fake);
    testing::internal::GetCapturedStdout();
    unsetenv("DEPOT_STRICT_PATHS");
    EXPECT_EQ(rc, 2);  // strict mode refuses to run on duplicates
}
