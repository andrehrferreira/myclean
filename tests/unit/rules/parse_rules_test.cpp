#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/rules/parse.hpp"
#include "core/rules/schema.hpp"

namespace myclean::rules {
namespace {

const std::filesystem::path rule_file = R"(C:\MyClean\data\rules\dev.toml)";

// A valid quarantine rule with `id`; `extra` lines are appended to its table.
std::string valid_rule(std::string_view id, std::string_view extra = {}) {
  return "[[rule]]\nid = \"" + std::string(id) +
         "\"\ncategory = \"caches\"\ntier = \"ask\"\nscope = \"user\"\n"
         "strategy = \"quarantine\"\ntargets = ['%TEMP%\\cache']\n" +
         std::string(extra) + "\n";
}

// Parses `text` expecting a rejection, and returns its message.
std::string rejection(std::string_view text) {
  const Result<std::vector<CleaningRule>> rules = parse_rule_file(text, rule_file);
  EXPECT_FALSE(rules.has_value());
  if (rules.has_value()) {
    return {};
  }
  EXPECT_EQ(rules.error().code, Errc::invalid_data);
  return rules.error().message;
}

TEST(ParseRules, ArchitectureExampleRule) {
  const Result<std::vector<CleaningRule>> rules = parse_rule_file(R"(
[[rule]]
id          = "dev.node.node_modules"
category    = "dev-artifacts"
tier        = "auto"
scope       = "user"
project     = { marker = "package.json" }
targets     = ["node_modules", ".next", ".turbo", ".parcel-cache"]
require     = { git_ignored = true, inactive_days = 30, not_in_use = true }
strategy    = "delete"
)",
                                                                  rule_file);
  ASSERT_TRUE(rules.has_value()) << rules.error().message;
  ASSERT_EQ(rules->size(), 1U);
  const CleaningRule& rule = rules->front();
  EXPECT_EQ(rule.id, "dev.node.node_modules");
  EXPECT_EQ(rule.category, "dev-artifacts");
  EXPECT_EQ(rule.tier, Tier::automatic);
  EXPECT_EQ(rule.scope, ExecutionContext::user);
  EXPECT_EQ(rule.strategy, Strategy::remove);
  EXPECT_EQ(rule.targets,
            (std::vector<std::string>{"node_modules", ".next", ".turbo", ".parcel-cache"}));
  EXPECT_EQ(rule.project_marker, "package.json");
  EXPECT_TRUE(rule.require.git_ignored);
  EXPECT_FALSE(rule.require.outside_git);
  EXPECT_TRUE(rule.require.not_in_use);
  EXPECT_EQ(rule.require.inactive_days, 30);
  EXPECT_FALSE(rule.require.min_age_days.has_value());
  EXPECT_TRUE(rule.command.empty());
  EXPECT_TRUE(rule.handler.empty());
}

TEST(ParseRules, NativeAndHandlerRulesWithSystemScope) {
  const Result<std::vector<CleaningRule>> rules = parse_rule_file(R"(
[[rule]]
id = "pkg.pnpm.store"
category = "package-managers"
tier = "ask"
scope = "user"
strategy = "native"
command = "pnpm store prune"

[[rule]]
id = "windows.temp"
category = "windows"
tier = "auto"
scope = "system"
strategy = "handler"
handler = "Temporary Files"
require = { min_age_days = 2 }
)",
                                                                  rule_file);
  ASSERT_TRUE(rules.has_value()) << rules.error().message;
  ASSERT_EQ(rules->size(), 2U);
  EXPECT_EQ((*rules)[0].strategy, Strategy::native);
  EXPECT_EQ((*rules)[0].command, "pnpm store prune");
  EXPECT_TRUE((*rules)[0].targets.empty());
  EXPECT_EQ((*rules)[1].scope, ExecutionContext::service);
  EXPECT_EQ((*rules)[1].strategy, Strategy::handler);
  EXPECT_EQ((*rules)[1].handler, "Temporary Files");
  EXPECT_EQ((*rules)[1].require.min_age_days, 2);
}

TEST(ParseRules, FileWithoutRulesIsEmpty) {
  const Result<std::vector<CleaningRule>> rules = parse_rule_file("# nothing yet\n", rule_file);
  ASSERT_TRUE(rules.has_value());
  EXPECT_TRUE(rules->empty());
}

// Store spec, "Invalid rule file rejected whole".
TEST(ParseRules, OneRuleMissingTierRejectsAllElevenNamingFileRuleAndField) {
  std::string text;
  for (int index = 1; index <= 10; ++index) {
    text += valid_rule("cache." + std::to_string(index));
  }
  text +=
      "[[rule]]\nid = \"cache.broken\"\ncategory = \"caches\"\nscope = \"user\"\n"
      "strategy = \"quarantine\"\ntargets = ['%TEMP%\\cache']\n";

  EXPECT_EQ(rejection(text),
            R"(C:\MyClean\data\rules\dev.toml: rule #11 'cache.broken': field 'tier' is missing)");
}

// Cleaning spec, "Invalid rule rejected".
TEST(ParseRules, UnknownStrategyValueRejected) {
  const std::string message = rejection(
      "[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"ask\"\nscope = \"user\"\n"
      "strategy = \"shred\"\ntargets = [\"t\"]\n");
  EXPECT_NE(message.find("rule #1 'x': field 'strategy' must be one of delete, quarantine, "
                         "native or handler, not 'shred'"),
            std::string::npos)
      << message;
}

TEST(ParseRules, UnknownTierAndScopeValuesRejected) {
  EXPECT_NE(rejection("[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"always\"\n")
                .find("field 'tier' must be one of auto, ask or report, not 'always'"),
            std::string::npos);
  EXPECT_NE(rejection("[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"ask\"\nscope = \"svc\"\n")
                .find("field 'scope' must be one of user or system, not 'svc'"),
            std::string::npos);
}

TEST(ParseRules, MisspelledKeyRejected) {
  const std::string message = rejection(valid_rule("x", "categry = \"caches\""));
  EXPECT_NE(message.find("rule #1 'x': field 'categry' is not a known field"), std::string::npos)
      << message;
}

TEST(ParseRules, MisspelledNestedKeyRejected) {
  const std::string message = rejection(valid_rule("x", "require = { inactive_day = 30 }"));
  EXPECT_NE(message.find("field 'require.inactive_day' is not a known field"), std::string::npos)
      << message;
  EXPECT_NE(rejection(valid_rule("x", "project = { markers = \"Cargo.toml\" }"))
                .find("field 'project.markers' is not a known field"),
            std::string::npos);
}

TEST(ParseRules, MisspelledTopLevelKeyRejected) {
  const std::string message = rejection("[[rules]]\nid = \"x\"\n");
  EXPECT_NE(message.find("dev.toml: field 'rules' is not a known field"), std::string::npos)
      << message;
}

TEST(ParseRules, DuplicateIdRejected) {
  const std::string message =
      rejection(valid_rule("same") + valid_rule("other") + valid_rule("same"));
  EXPECT_NE(message.find("rule #3 'same': field 'id' repeats an id used earlier in the file"),
            std::string::npos)
      << message;
}

TEST(ParseRules, MissingIdNamesThePosition) {
  const std::string message = rejection(valid_rule("a") + "[[rule]]\ncategory = \"c\"\n");
  EXPECT_NE(message.find("dev.toml: rule #2: field 'id' is missing"), std::string::npos) << message;
}

TEST(ParseRules, WrongTypesAndRangesRejected) {
  EXPECT_NE(rejection(valid_rule("x", "require = { inactive_days = \"30\" }"))
                .find("field 'require.inactive_days' must be an integer"),
            std::string::npos);
  EXPECT_NE(rejection(valid_rule("x", "require = { min_age_days = -1 }"))
                .find("field 'require.min_age_days' must be between 0 and 36500"),
            std::string::npos);
  EXPECT_NE(rejection(valid_rule("x", "require = { not_in_use = 1 }"))
                .find("field 'require.not_in_use' must be true or false"),
            std::string::npos);
  EXPECT_NE(rejection(valid_rule("x", "project = \"package.json\""))
                .find("field 'project' must be a table"),
            std::string::npos);
  EXPECT_NE(rejection("[[rule]]\nid = 7\n").find("field 'id' must be a string"), std::string::npos);
  EXPECT_NE(rejection("[[rule]]\nid = \"\"\n").find("field 'id' must not be empty"),
            std::string::npos);
}

TEST(ParseRules, TargetListsMustHoldNonEmptyStrings) {
  const std::string head =
      "[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"ask\"\nscope = \"user\"\n"
      "strategy = \"delete\"\n";
  EXPECT_NE(rejection(head + "targets = []\n").find("field 'targets' must be a non-empty list"),
            std::string::npos);
  EXPECT_NE(rejection(head + "targets = [\"a\", 2]\n").find("field 'targets' must be a non-empty"),
            std::string::npos);
  EXPECT_NE(rejection(head + "targets = \"a\"\n").find("field 'targets' must be a non-empty"),
            std::string::npos);
}

// A target the engine would never match rejects the file instead of loading and selecting
// nothing: absolute targets need a drive, "?:\" or a %VARIABLE%; project targets are relative.
TEST(ParseRules, TargetsTheEngineWouldDropRejected) {
  const std::string head =
      "[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"ask\"\nscope = \"user\"\n"
      "strategy = \"delete\"\n";
  const std::string project = "project = { marker = \"build.zig\" }\n";
  for (const char* target : {"cache", R"(\cache)", R"(C:cache)", R"(?:cache)"}) {
    EXPECT_NE(rejection(head + "targets = ['" + target + "']\n")
                  .find("field 'targets' must hold absolute paths starting with a drive, '?:\\' "
                        "or a %VARIABLE%, not '" +
                        std::string(target) + "'"),
              std::string::npos)
        << target;
  }
  for (const char* target : {R"(C:\out)", R"(?:\out)", R"(\out)", R"(%TEMP%\out)"}) {
    EXPECT_NE(rejection(head + project + "targets = ['" + target + "']\n")
                  .find("field 'targets' must hold paths relative to the project root, not '" +
                        std::string(target) + "'"),
              std::string::npos)
        << target;
  }
  EXPECT_NE(rejection(head + R"(targets = ['%TEMP%\..\x'])" + "\n")
                .find(R"(field 'targets' must not hold a '..' component, in '%TEMP%\..\x')"),
            std::string::npos);
  EXPECT_NE(rejection(head + project + "targets = ['out/../src']\n")
                .find("field 'targets' must not hold a '..' component"),
            std::string::npos);

  const Result<std::vector<CleaningRule>> valid = parse_rule_file(
      head + R"(targets = ['?:\Python3*\Lib\site-packages\~*', 'F:\x', '%TEMP%\y'])" + "\n" +
          "[[rule]]\nid = \"p\"\ncategory = \"c\"\ntier = \"ask\"\nscope = \"user\"\n"
          "strategy = \"delete\"\n" +
          project + R"(targets = ['.zig-cache', '.vs\*\v1*\ipch'])" + "\n",
      rule_file);
  ASSERT_TRUE(valid.has_value()) << valid.error().message;
  EXPECT_EQ(valid->front().targets.front(), R"(?:\Python3*\Lib\site-packages\~*)");
}

// require.outside_git widens git_ignored for project rules; anywhere else it would mean nothing.
TEST(ParseRules, OutsideGitOnlyOnProjectRulesWithGitIgnored) {
  const std::string head =
      "[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"auto\"\nscope = \"user\"\n"
      "strategy = \"delete\"\n";
  const std::string project = "project = { marker = \"*.sln\" }\ntargets = ['.vs']\n";
  const Result<std::vector<CleaningRule>> rules = parse_rule_file(
      head + project + "require = { git_ignored = true, outside_git = true }\n", rule_file);
  ASSERT_TRUE(rules.has_value()) << rules.error().message;
  EXPECT_TRUE(rules->front().require.outside_git);
  EXPECT_TRUE(rules->front().require.git_ignored);

  for (const std::string& rule :
       {head + project + "require = { outside_git = true }\n",
        head + "targets = ['%TEMP%\\x']\nrequire = { git_ignored = true, outside_git = true }\n"}) {
    EXPECT_NE(rejection(rule).find("field 'require.outside_git' is only allowed on a project rule "
                                   "with require.git_ignored"),
              std::string::npos)
        << rule;
  }
}

// require.contains narrows a project rule's matches to folders holding one named file.
TEST(ParseRules, ContainsOnlyOnProjectRulesAndNamesOneFile) {
  const std::string head =
      "[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"auto\"\nscope = \"user\"\n"
      "strategy = \"delete\"\n";
  const std::string project = "project = { marker = \"CMakeLists.txt\" }\ntargets = ['build\\*']\n";
  const Result<std::vector<CleaningRule>> rules =
      parse_rule_file(head + project + "require = { contains = \"CMakeCache.txt\" }\n", rule_file);
  ASSERT_TRUE(rules.has_value()) << rules.error().message;
  EXPECT_EQ(rules->front().require.contains, "CMakeCache.txt");

  for (const std::string& rule :
       {head + "targets = ['%TEMP%\\x']\nrequire = { contains = \"CMakeCache.txt\" }\n",
        head + project + "require = { contains = \"CMakeFiles\\\\x.txt\" }\n",
        head + project + "require = { contains = \"*.txt\" }\n"}) {
    EXPECT_NE(rejection(rule).find(
                  "field 'require.contains' is only allowed on a project rule and names one file"),
              std::string::npos)
        << rule;
  }
}

TEST(ParseRules, StrategySpecificFieldsEnforced) {
  const std::string head =
      "[[rule]]\nid = \"x\"\ncategory = \"c\"\ntier = \"ask\"\nscope = \"user\"\n";
  EXPECT_NE(rejection(head + "strategy = \"delete\"\n")
                .find("field 'targets' is required for strategy 'delete'"),
            std::string::npos);
  EXPECT_NE(rejection(head + "strategy = \"native\"\n")
                .find("field 'command' is required for strategy 'native'"),
            std::string::npos);
  EXPECT_NE(rejection(head + "strategy = \"handler\"\n")
                .find("field 'handler' is required for strategy 'handler'"),
            std::string::npos);
  EXPECT_NE(rejection(head + "strategy = \"delete\"\ntargets = [\"t\"]\ncommand = \"rm\"\n")
                .find("field 'command' is only allowed with strategy 'native'"),
            std::string::npos);
  EXPECT_NE(rejection(head + "strategy = \"native\"\ncommand = \"c\"\nhandler = \"h\"\n")
                .find("field 'handler' is only allowed with strategy 'handler'"),
            std::string::npos);
}

TEST(ParseRules, TomlSyntaxErrorNamesFileAndLine) {
  const std::string message = rejection(valid_rule("x") + "tier = \n");
  EXPECT_NE(message.find(R"x(C:\MyClean\data\rules\dev.toml: is not valid TOML (line 9,)x"),
            std::string::npos)
      << message;
}

TEST(Strategy, TextFormsRoundTrip) {
  for (const Strategy strategy :
       {Strategy::remove, Strategy::quarantine, Strategy::native, Strategy::handler}) {
    EXPECT_EQ(parse_strategy(to_string(strategy)), strategy);
  }
  EXPECT_EQ(to_string(Strategy::remove), "delete");
  EXPECT_FALSE(parse_strategy("Delete").has_value());
}

}  // namespace
}  // namespace myclean::rules
