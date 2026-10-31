#include "core/alerts/config.hpp"

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::alerts {
namespace {

Result<AlertConfig> parse(const std::string& text) {
  return parse_alert_file(text, "C:\\data\\alerts.toml");
}

// The error message of a file that must be rejected.
std::string rejection(const std::string& text) {
  const Result<AlertConfig> config = parse(text);
  EXPECT_FALSE(config.has_value());
  if (config) {
    return {};
  }
  EXPECT_EQ(config.error().code, Errc::invalid_data);
  return config.error().message;
}

TEST(AlertConfig, ThresholdAndChangeRules) {
  const Result<AlertConfig> config = parse(R"(
rate_limit_minutes = 7

[[rule]]
id = "volume.free-space-low"
title = "Drive is almost full"
metric = "volume.free_percent"
severity = "warning"
below = 10
unit = "%"

[[rule]]
id = "disk.temperature"
title = "Drive is hot"
metric = "disk.temperature"
severity = "critical"
above = 69.5

[[rule]]
id = "disk.media-errors"
title = "New media errors"
metric = "disk.media_errors"
severity = "info"
change = "any"
)");
  ASSERT_TRUE(config.has_value()) << config.error().message;
  EXPECT_EQ(config->rate_limit, std::chrono::minutes{7});
  ASSERT_EQ(config->rules.size(), 3U);
  EXPECT_EQ(config->rules[0].id, "volume.free-space-low");
  EXPECT_EQ(config->rules[0].title, "Drive is almost full");
  EXPECT_EQ(config->rules[0].metric, "volume.free_percent");
  EXPECT_EQ(config->rules[0].severity, Severity::warning);
  EXPECT_EQ(config->rules[0].below, 10.0);
  EXPECT_EQ(config->rules[0].unit, "%");
  EXPECT_EQ(config->rules[1].above, 69.5);
  EXPECT_FALSE(config->rules[1].below.has_value());
  EXPECT_EQ(config->rules[1].severity, Severity::critical);
  EXPECT_EQ(config->rules[2].change, Change::any);
  EXPECT_EQ(config->rules[2].severity, Severity::info);
  EXPECT_TRUE(config->rules[2].unit.empty());
}

TEST(AlertConfig, DefaultsAndEmptyFile) {
  const Result<AlertConfig> config = parse("");
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->rate_limit, std::chrono::minutes{5});
  EXPECT_TRUE(config->rules.empty());
}

TEST(AlertConfig, MissingTitleNamesFileRuleAndField) {
  EXPECT_EQ(rejection(R"(
[[rule]]
id = "a"
metric = "m"
severity = "warning"
below = 1
)"),
            "C:\\data\\alerts.toml: rule #1 'a': field 'title' is missing");
}

TEST(AlertConfig, ExactlyOneConditionPerRule) {
  const std::string head =
      "[[rule]]\nid = \"a\"\ntitle = \"t\"\nmetric = \"m\"\nseverity = \"info\"\n";
  EXPECT_NE(rejection(head).find("exactly one of below, above and change"), std::string::npos);
  EXPECT_NE(rejection(head + "below = 1\nabove = 2\n").find("exactly one"), std::string::npos);
  EXPECT_NE(rejection(head + "below = 1\nchange = \"any\"\n").find("exactly one"),
            std::string::npos);
}

TEST(AlertConfig, InvalidValuesRejected) {
  const std::string head = "[[rule]]\nid = \"a\"\ntitle = \"t\"\nmetric = \"m\"\n";
  EXPECT_NE(rejection(head + "severity = \"fatal\"\nbelow = 1\n").find("field 'severity'"),
            std::string::npos);
  EXPECT_NE(rejection(head + "severity = \"info\"\nchange = \"up\"\n").find("field 'change'"),
            std::string::npos);
  EXPECT_NE(rejection(head + "severity = \"info\"\nbelow = \"low\"\n").find("must be a number"),
            std::string::npos);
  EXPECT_NE(rejection(head + "severity = \"info\"\nbelow = 1\nlimit = 2\n")
                .find("field 'limit' is not a known field"),
            std::string::npos);
  EXPECT_NE(rejection("rate_limit_minutes = 0\n").find("rate_limit_minutes"), std::string::npos);
  EXPECT_NE(rejection("rate_limit_minutes = 2.5\n").find("rate_limit_minutes"), std::string::npos);
  EXPECT_NE(rejection("rule = 3\n").find("[[rule]]"), std::string::npos);
  EXPECT_NE(rejection("[[rule]\n").find("is not valid TOML"), std::string::npos);
}

TEST(AlertConfig, DuplicateIdRejected) {
  const std::string rule =
      "[[rule]]\nid = \"a\"\ntitle = \"t\"\nmetric = \"m\"\nseverity = \"info\"\nbelow = 1\n";
  EXPECT_NE(rejection(rule + rule).find("rule #2 'a': field 'id' repeats"), std::string::npos);
}

TEST(AlertConfig, ShippedFileLoads) {
  const Result<AlertConfig> config = load_alert_config(rules::DiskDataFiles{}, MYCLEAN_DATA_DIR);
  ASSERT_TRUE(config.has_value()) << config.error().message;
  EXPECT_FALSE(config->rules.empty());
}

}  // namespace
}  // namespace myclean::alerts
