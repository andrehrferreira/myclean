#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

// The alert rules, loaded from data/alerts.toml (alerts spec, "Alert Rules"):
//
//   rate_limit_minutes = 5                    # optional, 1..1440, default 5
//
//   [[rule]]
//   id       = "volume.fill-up"               # required, unique
//   title    = "Drive will be full soon"      # required, plain language, shown in the toast
//   metric   = "volume.days_until_full"       # required: the reading the rule watches
//   severity = "warning"                      # required: info | warning | critical
//   below    = 14.0                           # threshold rules: exactly one of below, above
//   above    = 70.0
//   change   = "increase"                     # change rules instead: increase | any
//   unit     = "days"                         # optional, for display
//
// A threshold rule fires when a reading goes beyond its limit, a change rule when a reading
// differs from the previous one of the same entity (increase: only when it is higher).
namespace myclean::alerts {

enum class Severity { info, warning, critical };

[[nodiscard]] std::string_view to_string(Severity severity);
[[nodiscard]] std::optional<Severity> parse_severity(std::string_view text);

enum class Change { increase, any };

struct AlertRule {
  std::string id;
  std::string title;
  std::string metric;
  Severity severity = Severity::warning;
  std::optional<double> below;
  std::optional<double> above;
  std::optional<Change> change;  // set for change rules, which have no limit
  std::string unit;
};

struct AlertConfig {
  std::chrono::minutes rate_limit{5};
  std::vector<AlertRule> rules;
};

// Validates the whole file: every rule or one Errc::invalid_data error naming the file, the
// rule and the field. `file` is used only in messages.
[[nodiscard]] Result<AlertConfig> parse_alert_file(std::string_view text,
                                                   const std::filesystem::path& file);

// <data_dir>/alerts.toml
[[nodiscard]] Result<AlertConfig> load_alert_config(const rules::IDataFiles& files,
                                                    const std::filesystem::path& data_dir);

}  // namespace myclean::alerts
