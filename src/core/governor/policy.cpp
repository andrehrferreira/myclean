#include "core/governor/policy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/rules/data_files.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governor {

namespace {

constexpr double gib = 1024.0 * 1024.0 * 1024.0;
constexpr int max_seconds = 3600;
constexpr int max_minutes = 1440;
constexpr int max_commit_gib = 1 << 20;

// Thrown at the first schema violation; parse_policy_file returns it as invalid_data.
class SchemaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return {text.begin(), text.end()};
}

// Validated access to the keys of one table. Messages read "<file>: field '<prefix><key>' ...".
class Fields {
 public:
  Fields(const toml::table& table, std::string file, std::string prefix = {})
      : table_(&table), file_(std::move(file)), prefix_(std::move(prefix)) {}

  [[noreturn]] void fail(std::string_view key, std::string_view problem) const {
    throw SchemaError(file_ + ": field '" + prefix_ + std::string(key) + "' " +
                      std::string(problem));
  }

  void only(std::initializer_list<std::string_view> allowed) const {
    for (const auto& [key, node] : *table_) {
      if (std::ranges::find(allowed, key.str()) == allowed.end()) {
        fail(key.str(), "is not a known field");
      }
    }
  }

  // The fields of a nested table; std::nullopt when absent.
  [[nodiscard]] std::optional<Fields> table(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (!node->is_table()) {
      fail(key, "must be a table");
    }
    return Fields(*node->as_table(), file_, prefix_ + std::string(key) + ".");
  }

  // Integers are accepted where a number is expected ("cpu_percent = 90").
  [[nodiscard]] std::optional<double> number(std::string_view key, double min, double max) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    double value = 0.0;
    if (node->is_integer()) {
      value = static_cast<double>(node->as_integer()->get());
    } else if (node->is_floating_point()) {
      value = node->as_floating_point()->get();
    } else {
      fail(key, "must be a number");
    }
    if (!std::isfinite(value) || value < min || value > max) {
      fail(key, "must be between " + text(min) + " and " + text(max));
    }
    return value;
  }

  [[nodiscard]] std::optional<int> integer(std::string_view key, int min, int max) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    if (!node->is_integer()) {
      fail(key, "must be a whole number");
    }
    const std::int64_t value = node->as_integer()->get();
    if (value < min || value > max) {
      fail(key, "must be between " + std::to_string(min) + " and " + std::to_string(max));
    }
    return static_cast<int>(value);
  }

  // A list of strings, which may be empty; std::nullopt when absent.
  [[nodiscard]] std::optional<std::vector<std::string>> strings(std::string_view key) const {
    const toml::node* node = table_->get(key);
    if (node == nullptr) {
      return std::nullopt;
    }
    const toml::array* array = node->as_array();
    if (array == nullptr) {
      fail(key, "must be a list of strings");
    }
    std::vector<std::string> values;
    for (const toml::node& element : *array) {
      if (!element.is_string()) {
        fail(key, "must be a list of strings");
      }
      values.push_back(element.as_string()->get());
    }
    return values;
  }

 private:
  static std::string text(double value) {
    std::string printed = std::to_string(value);
    printed.erase(printed.find_last_not_of('0') + 1);
    if (printed.back() == '.') {
      printed.pop_back();
    }
    return printed;
  }

  const toml::table* table_;
  std::string file_;
  std::string prefix_;
};

template <typename Duration>
void read_duration(const Fields& fields, std::string_view key, int max, Duration& value) {
  if (const std::optional<int> count = fields.integer(key, 1, max)) {
    value = Duration{*count};
  }
}

void read_percent(const Fields& fields, std::string_view key, double& value) {
  value = fields.number(key, 1.0, 100.0).value_or(value);
}

void read_gib(const Fields& fields, std::string_view key, Bytes& value) {
  if (const std::optional<double> amount = fields.number(key, 0.0, max_commit_gib)) {
    value = Bytes{static_cast<std::uint64_t>(std::llround(*amount * gib))};
  }
}

void read_pressure(const Fields& root, PressurePolicy& policy) {
  if (const std::optional<Fields> fields = root.table("pressure")) {
    fields->only({"cpu_percent", "cpu_seconds", "available_gib", "commit_percent",
                  "hard_faults_per_second", "hard_faults_seconds"});
    read_percent(*fields, "cpu_percent", policy.cpu_enter_percent);
    read_duration(*fields, "cpu_seconds", max_seconds, policy.cpu_dwell);
    read_gib(*fields, "available_gib", policy.available_enter);
    read_percent(*fields, "commit_percent", policy.commit_enter_percent);
    policy.hard_faults_enter =
        fields->number("hard_faults_per_second", 1.0, 1e9).value_or(policy.hard_faults_enter);
    read_duration(*fields, "hard_faults_seconds", max_seconds, policy.hard_faults_dwell);
  }
  policy.hard_faults_exit = policy.hard_faults_enter / 2.0;
  const std::optional<Fields> fields = root.table("clear");
  if (!fields) {
    return;
  }
  fields->only(
      {"cpu_percent", "available_gib", "commit_percent", "hard_faults_per_second", "seconds"});
  read_percent(*fields, "cpu_percent", policy.cpu_exit_percent);
  read_gib(*fields, "available_gib", policy.available_exit);
  read_percent(*fields, "commit_percent", policy.commit_exit_percent);
  policy.hard_faults_exit =
      fields->number("hard_faults_per_second", 0.0, 1e9).value_or(policy.hard_faults_exit);
  read_duration(*fields, "seconds", max_seconds, policy.clear_after);
}

// The exit thresholds must lie on the calm side of the entry thresholds, or the state would
// never settle.
void check_hysteresis(const Fields& root, const PressurePolicy& policy) {
  if (policy.cpu_exit_percent > policy.cpu_enter_percent) {
    root.fail("clear.cpu_percent", "must not be above pressure.cpu_percent");
  }
  if (policy.available_exit < policy.available_enter) {
    root.fail("clear.available_gib", "must not be below pressure.available_gib");
  }
  if (policy.commit_exit_percent > policy.commit_enter_percent) {
    root.fail("clear.commit_percent", "must not be above pressure.commit_percent");
  }
  if (policy.hard_faults_exit > policy.hard_faults_enter) {
    root.fail("clear.hard_faults_per_second", "must not be above pressure.hard_faults_per_second");
  }
}

void read_ladder(const Fields& root, LadderPolicy& policy) {
  const std::optional<Fields> fields = root.table("ladder");
  if (!fields) {
    return;
  }
  fields->only({"cooldown_seconds", "cap_cpu_percent", "cap_commit_headroom_percent",
                "cap_commit_min_headroom_mib", "resume_blocks_suspend_minutes", "kill"});
  read_duration(*fields, "cooldown_seconds", max_seconds, policy.cooldown);
  policy.cap_cpu_percent =
      fields->integer("cap_cpu_percent", 1, 100).value_or(policy.cap_cpu_percent);
  policy.cap_commit_headroom_percent = fields->integer("cap_commit_headroom_percent", 0, 1000)
                                           .value_or(policy.cap_commit_headroom_percent);
  if (const std::optional<int> mib =
          fields->integer("cap_commit_min_headroom_mib", 64, max_commit_gib)) {
    policy.cap_commit_min_headroom = Bytes{static_cast<std::uint64_t>(*mib) << 20U};
  }
  read_duration(*fields, "resume_blocks_suspend_minutes", max_minutes,
                policy.resume_blocks_suspend);
  if (const std::optional<std::vector<std::string>> kill = fields->strings("kill")) {
    policy.kill.clear();
    for (const std::string& name : *kill) {
      const std::optional<rules::ProcessClass> parsed = rules::parse_process_class(name);
      if (!parsed ||
          (*parsed != rules::ProcessClass::build && *parsed != rules::ProcessClass::test &&
           *parsed != rules::ProcessClass::normal)) {
        fields->fail("kill", "must list only build, test or default, not '" + name + "'");
      }
      if (std::ranges::find(policy.kill, *parsed) == policy.kill.end()) {
        policy.kill.push_back(*parsed);
      }
    }
  }
}

void read_caps(const Fields& jobs, std::string_view name, rules::JobCaps& caps) {
  const std::optional<Fields> fields = jobs.table(name);
  if (!fields) {
    return;
  }
  fields->only({"cpu_percent", "commit_gib"});
  caps.cpu_percent = fields->integer("cpu_percent", 1, 100).value_or(caps.cpu_percent);
  if (const std::optional<int> commit = fields->integer("commit_gib", 1, max_commit_gib)) {
    caps.commit = Bytes{static_cast<std::uint64_t>(*commit) << 30U};
  }
}

void read_jobs(const Fields& root, GovernorPolicy& policy) {
  if (const std::optional<Fields> game = root.table("game_mode")) {
    game->only({"pressure_seconds", "cooldown_seconds", "build_cpu_percent"});
    read_duration(*game, "pressure_seconds", max_seconds, policy.pressure.game_dwell);
    read_duration(*game, "cooldown_seconds", max_seconds, policy.ladder.game_cooldown);
    policy.jobs.game_build_cpu_percent =
        game->integer("build_cpu_percent", 1, 100).value_or(policy.jobs.game_build_cpu_percent);
  }
  if (const std::optional<Fields> jobs = root.table("jobs")) {
    jobs->only({"notification_percent", "rearm_percent", "build", "test"});
    policy.jobs.notification_percent =
        jobs->integer("notification_percent", 1, 100).value_or(policy.jobs.notification_percent);
    policy.jobs.rearm_percent =
        jobs->integer("rearm_percent", 1, 100).value_or(policy.jobs.rearm_percent);
    if (policy.jobs.rearm_percent >= policy.jobs.notification_percent) {
      jobs->fail("rearm_percent", "must be below notification_percent");
    }
    read_caps(*jobs, "build", policy.jobs.build);
    read_caps(*jobs, "test", policy.jobs.test);
  }
  if (const std::optional<Fields> notifications = root.table("notifications")) {
    notifications->only({"interval_minutes"});
    read_duration(*notifications, "interval_minutes", max_minutes,
                  policy.ladder.notification_interval);
  }
}

}  // namespace

bool kill_enabled(const LadderPolicy& policy, rules::ProcessClass process_class) {
  return std::ranges::find(policy.kill, process_class) != policy.kill.end();
}

Result<GovernorPolicy> parse_policy_file(std::string_view text, const std::filesystem::path& file) {
  const std::string name = utf8(file);
  toml::table root;
  try {
    root = toml::parse(text, name);
  } catch (const toml::parse_error& error) {
    const toml::source_position& where = error.source().begin;
    return fail(Errc::invalid_data,
                name + ": is not valid TOML (line " + std::to_string(where.line) + ", column " +
                    std::to_string(where.column) + "): " + std::string(error.description()));
  }
  try {
    const Fields fields(root, name);
    fields.only({"pressure", "clear", "ladder", "game_mode", "jobs", "notifications"});
    GovernorPolicy policy;
    read_pressure(fields, policy.pressure);
    check_hysteresis(fields, policy.pressure);
    read_ladder(fields, policy.ladder);
    read_jobs(fields, policy);
    return policy;
  } catch (const SchemaError& error) {
    return fail(Errc::invalid_data, error.what());
  }
}

Result<GovernorPolicy> load_policy(const rules::IDataFiles& files,
                                   const std::filesystem::path& data_dir) {
  const std::filesystem::path file = data_dir / "governor.toml";
  return files.read(file).and_then(
      [&file](const std::string& text) { return parse_policy_file(text, file); });
}

}  // namespace myclean::governor
