#include "core/alerts/config.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/rules/data_files.hpp"

namespace myclean::alerts {

namespace {

constexpr std::array<std::string_view, 3> severity_names = {"info", "warning", "critical"};
constexpr std::array<std::string_view, 2> change_names = {"increase", "any"};

// Thrown at the first schema violation; parse_alert_file returns it as invalid_data.
class SchemaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// Validated access to the keys of one table; `where` starts every message.
class Fields {
 public:
  Fields(const toml::table& table, std::string where) : table_(&table), where_(std::move(where)) {}

  void set_id(std::string_view id) { where_ += " '" + std::string(id) + "'"; }

  // "<where>: field '<key>' <problem>", or "<where>: <problem>" when `key` is empty.
  [[noreturn]] void fail(std::string_view key, std::string_view problem) const {
    if (key.empty()) {
      throw SchemaError(where_ + ": " + std::string(problem));
    }
    throw SchemaError(where_ + ": field '" + std::string(key) + "' " + std::string(problem));
  }

  void only(std::initializer_list<std::string_view> allowed) const {
    for (const auto& [key, node] : *table_) {
      if (std::ranges::find(allowed, key.str()) == allowed.end()) {
        fail(key.str(), "is not a known field");
      }
    }
  }

  [[nodiscard]] std::optional<std::string> string(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (!node->is_string() || node->as_string()->get().empty()) {
      fail(key, "must be a non-empty string");
    }
    return node->as_string()->get();
  }

  [[nodiscard]] std::string required_string(std::string_view key) const {
    std::optional<std::string> value = string(key);
    if (!value) {
      fail(key, "is missing");
    }
    return *std::move(value);
  }

  // Integers are accepted where a number is expected ("below = 10").
  [[nodiscard]] std::optional<double> number(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (node->is_integer()) {
      return static_cast<double>(node->as_integer()->get());
    }
    if (!node->is_floating_point()) {
      fail(key, "must be a number");
    }
    return node->as_floating_point()->get();
  }

 private:
  const toml::table* table_;
  std::string where_;
};

AlertRule parse_rule(Fields& fields) {
  AlertRule rule;
  rule.id = fields.required_string("id");
  fields.set_id(rule.id);
  fields.only({"id", "title", "metric", "severity", "below", "above", "change", "unit"});
  rule.title = fields.required_string("title");
  rule.metric = fields.required_string("metric");
  const std::string severity = fields.required_string("severity");
  const std::optional<Severity> parsed = parse_severity(severity);
  if (!parsed) {
    fields.fail("severity", "must be one of info, warning, critical, not '" + severity + "'");
  }
  rule.severity = *parsed;
  rule.below = fields.number("below");
  rule.above = fields.number("above");
  if (const std::optional<std::string> change = fields.string("change")) {
    rule.change = detail::enum_from_name<Change>(change_names, *change);
    if (!rule.change) {
      fields.fail("change", "must be one of increase, any, not '" + *change + "'");
    }
  }
  if (int{rule.below.has_value()} + int{rule.above.has_value()} + int{rule.change.has_value()} !=
      1) {
    fields.fail("", "must set exactly one of below, above and change");
  }
  rule.unit = fields.string("unit").value_or("");
  return rule;
}

}  // namespace

std::string_view to_string(Severity severity) {
  return detail::enum_name(severity_names, severity);
}

std::optional<Severity> parse_severity(std::string_view text) {
  return detail::enum_from_name<Severity>(severity_names, text);
}

Result<AlertConfig> parse_alert_file(std::string_view text, const std::filesystem::path& file) {
  const std::string name = utf8(file);
  try {
    toml::table root;
    try {
      root = toml::parse(text, name);
    } catch (const toml::parse_error& error) {
      return fail(Errc::invalid_data,
                  name + ": is not valid TOML: " + std::string(error.description()));
    }
    const Fields root_fields(root, name);
    root_fields.only({"rate_limit_minutes", "rule"});
    AlertConfig config;
    if (const std::optional<double> minutes = root_fields.number("rate_limit_minutes")) {
      if (*minutes < 1 || *minutes > 1440 ||
          *minutes != static_cast<double>(static_cast<int>(*minutes))) {
        root_fields.fail("rate_limit_minutes", "must be a whole number between 1 and 1440");
      }
      config.rate_limit = std::chrono::minutes{static_cast<int>(*minutes)};
    }
    const toml::node* rules = root.get("rule");
    if (rules != nullptr && !rules->is_array_of_tables()) {
      root_fields.fail("rule", "must be an array of tables ([[rule]])");
    }
    std::set<std::string, std::less<>> ids;
    if (rules != nullptr) {
      for (const toml::node& node : *rules->as_array()) {
        Fields fields(*node.as_table(),
                      name + ": rule #" + std::to_string(config.rules.size() + 1));
        config.rules.push_back(parse_rule(fields));
        if (!ids.insert(config.rules.back().id).second) {
          fields.fail("id", "repeats an id used earlier in the file");
        }
      }
    }
    return config;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

Result<AlertConfig> load_alert_config(const rules::IDataFiles& files,
                                      const std::filesystem::path& data_dir) {
  const std::filesystem::path file = data_dir / "alerts.toml";
  return files.read(file).and_then(
      [&file](const std::string& text) { return parse_alert_file(text, file); });
}

}  // namespace myclean::alerts
