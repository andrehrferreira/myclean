#include "core/security/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model/enum_names.hpp"
#include "core/startup/startup.hpp"

namespace myclean::security {
namespace {

constexpr std::array<std::string_view, 11> source_names = {
    "run",     "run32", "runonce", "runonce32", "folder", "task",
    "service", "wmi",   "ifeo",    "winlogon",  "appinit"};

constexpr std::string_view blanks = " \t";

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequal(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

bool starts_with_iequal(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && iequal(text.substr(0, prefix.size()), prefix);
}

bool ends_with_iequal(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && iequal(text.substr(text.size() - suffix.size()), suffix);
}

std::size_t ifind(std::string_view text, std::string_view needle, std::size_t from) {
  for (std::size_t at = from; at + needle.size() <= text.size(); ++at) {
    if (iequal(text.substr(at, needle.size()), needle)) {
      return at;
    }
  }
  return std::string_view::npos;
}

bool blank(char c) {
  return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

std::string_view file_name(std::string_view path) {
  const std::size_t separator = path.find_last_of(R"(\/)");
  return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

// The executable of `command` and the rest of the line after it.
std::pair<std::string_view, std::string_view> split_command(std::string_view command) {
  constexpr std::string_view exe = ".exe";
  command = trim(command);
  if (command.empty()) {
    return {};
  }
  if (command.front() == '"') {
    command.remove_prefix(1);
    const std::size_t close = command.find('"');
    if (close == std::string_view::npos) {
      return {command, {}};
    }
    return {command.substr(0, close), command.substr(close + 1)};
  }
  std::size_t end = command.find_first_of(blanks);
  for (std::size_t at = ifind(command, exe, 0); at != std::string_view::npos;
       at = ifind(command, exe, at + 1)) {
    const std::size_t after = at + exe.size();
    if (after == command.size() || blank(command[after])) {
      end = after;
      break;
    }
  }
  if (end == std::string_view::npos) {
    return {command, {}};
  }
  return {command.substr(0, end), command.substr(end)};
}

std::string normalized(std::string_view executable) {
  constexpr std::string_view nt_prefix = R"(\??\)";
  constexpr std::string_view system_root = R"(\SystemRoot\)";
  if (starts_with_iequal(executable, nt_prefix)) {
    executable.remove_prefix(nt_prefix.size());
  }
  if (starts_with_iequal(executable, system_root)) {
    return R"(%SystemRoot%\)" + std::string(executable.substr(system_root.size()));
  }
  return std::string(executable);
}

PointSource point_source(startup::Source source) {
  switch (source) {
    case startup::Source::run:
      return PointSource::run;
    case startup::Source::run32:
      return PointSource::run32;
    case startup::Source::run_once:
      return PointSource::run_once;
    case startup::Source::run_once32:
      return PointSource::run_once32;
    case startup::Source::startup_folder:
      return PointSource::startup_folder;
    case startup::Source::task:
      return PointSource::task;
    case startup::Source::service:
      return PointSource::service;
  }
  return PointSource::run;
}

std::string_view hive(startup::Scope scope) {
  return scope == startup::Scope::user ? "HKCU" : "HKLM";
}

std::string location_of(const startup::Item& item) {
  const std::string root(hive(item.scope));
  switch (item.source) {
    case startup::Source::run:
      return root + R"(\Software\Microsoft\Windows\CurrentVersion\Run)";
    case startup::Source::run32:
      return root + R"(\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run)";
    case startup::Source::run_once:
      return root + R"(\Software\Microsoft\Windows\CurrentVersion\RunOnce)";
    case startup::Source::run_once32:
      return root + R"(\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\RunOnce)";
    case startup::Source::startup_folder:
      return item.scope == startup::Scope::user ? "Startup folder (user)"
                                                : "Startup folder (all users)";
    case startup::Source::task:
      return "Task Scheduler";
    case startup::Source::service:
      return R"(HKLM\SYSTEM\CurrentControlSet\Services)";
  }
  return {};
}

std::string software(bool wow64) {
  return wow64 ? R"(HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\)"
               : R"(HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\)";
}

// Windows' own subscription: the SCM event log filter bound to its event log consumer.
bool standard_subscription(const WmiSubscription& subscription) {
  return subscription.filter == "SCM Event Log Filter" &&
         subscription.consumer == "SCM Event Log Consumer" &&
         subscription.consumer_class == "NTEventLogEventConsumer";
}

bool runs_something(const WmiSubscription& subscription) {
  return subscription.consumer_class == "CommandLineEventConsumer" ||
         subscription.consumer_class == "ActiveScriptEventConsumer";
}

Point wmi_point(const WmiSubscription& subscription) {
  const bool standard = standard_subscription(subscription);
  Point point{
      .source = PointSource::wmi,
      .location = R"(root\subscription)",
      .name = (subscription.filter.empty() ? std::string("(no filter)") : subscription.filter) +
              " -> " + subscription.consumer_class + " " + subscription.consumer,
      .command = subscription.command,
      .standard = standard,
      .enabled = !subscription.filter.empty()};
  if (subscription.consumer_class == "ActiveScriptEventConsumer") {
    // ScriptFileName is a file; inline script text runs no file of its own.
    const std::string_view command = trim(subscription.command);
    const bool file = !command.empty() && command.find('\n') == std::string_view::npos &&
                      (ends_with_iequal(command, ".vbs") || ends_with_iequal(command, ".js") ||
                       ends_with_iequal(command, ".wsf"));
    point.target = file ? std::string(command) : std::string();
  } else if (subscription.consumer_class == "CommandLineEventConsumer") {
    point.target = target_of(subscription.command);
  }
  if (!standard && runs_something(subscription)) {
    point.concern = Concern::wmi_consumer;
  }
  return point;
}

// The DLLs of AppInit_DLLs: separated by spaces or commas, quotes allowed around a name.
std::vector<std::string> appinit_dlls(std::string_view dlls) {
  std::vector<std::string> names;
  std::string current;
  bool quoted = false;
  const auto flush = [&] {
    if (!current.empty()) {
      names.push_back(std::move(current));
      current.clear();
    }
  };
  for (const char c : dlls) {
    if (c == '"') {
      quoted = !quoted;
    } else if (!quoted && (c == ' ' || c == ',' || c == '\t')) {
      flush();
    } else {
      current.push_back(c);
    }
  }
  flush();
  return names;
}

// Userinit runs each comma-separated program: the first that is not Windows' userinit.exe, else
// userinit.exe itself.
std::string userinit_target(std::string_view value, std::string_view system_folder) {
  std::string_view first;
  for (std::size_t start = 0; start <= value.size();) {
    const std::size_t comma = std::min(value.find(',', start), value.size());
    const std::string_view entry = trim(value.substr(start, comma - start));
    if (!entry.empty()) {
      if (!default_userinit(entry, system_folder)) {
        return target_of(entry);
      }
      if (first.empty()) {
        first = entry;
      }
    }
    start = comma + 1;
  }
  return target_of(first);
}

}  // namespace

std::string_view to_string(PointSource source) {
  return detail::enum_name(source_names, source);
}

std::string target_of(std::string_view command) {
  const auto [executable, rest] = split_command(command);
  std::string target = normalized(executable);
  if (iequal(file_name(target), "rundll32.exe") || iequal(file_name(target), "rundll32")) {
    std::string_view argument = trim(rest);
    if (!argument.empty() && argument.front() == '"') {
      argument.remove_prefix(1);
      argument = argument.substr(0, argument.find('"'));
    } else {
      argument = argument.substr(0, argument.find_first_of(", \t"));
    }
    if (!argument.empty()) {
      return normalized(trim(argument));
    }
  }
  return target;
}

bool default_userinit(std::string_view value, std::string_view system_folder) {
  std::string_view text = trim(value);
  while (!text.empty() && (text.back() == ',' || blank(text.back()))) {
    text.remove_suffix(1);
  }
  if (iequal(text, "userinit.exe") || iequal(text, "userinit")) {
    return true;
  }
  std::string expected(system_folder);
  if (!expected.empty() && expected.back() != '\\') {
    expected.push_back('\\');
  }
  return !system_folder.empty() && iequal(text, expected + "userinit.exe");
}

std::vector<Point> persistence_points(std::span<const startup::Item> items,
                                      const PersistenceSnapshot& extra) {
  std::vector<Point> points;
  points.reserve(items.size() + extra.wmi.size() + extra.ifeo.size() + extra.winlogon.size() +
                 extra.appinit.size());
  for (const startup::Item& item : items) {
    points.push_back(Point{.source = point_source(item.source),
                           .location = location_of(item),
                           .name = item.display_name,
                           .command = item.command,
                           .target = target_of(item.command),
                           .enabled = item.enabled,
                           .signer = item.signer,
                           .startup_item = item.id});
  }
  for (const WmiSubscription& subscription : extra.wmi) {
    points.push_back(wmi_point(subscription));
  }
  for (const IfeoDebugger& debugger : extra.ifeo) {
    points.push_back(Point{.source = PointSource::ifeo,
                           .location = software(debugger.wow64) + "Image File Execution Options",
                           .name = debugger.image,
                           .command = debugger.debugger,
                           .target = target_of(debugger.debugger),
                           .enabled = true,
                           .concern = Concern::ifeo_debugger});
  }
  for (const WinlogonValue& value : extra.winlogon) {
    const bool shell = iequal(value.name, "Shell");
    const bool standard = shell ? iequal(trim(value.value), "explorer.exe")
                                : default_userinit(value.value, extra.system_folder);
    Point point{.source = PointSource::winlogon,
                .location = std::string(hive(value.scope)) +
                            R"(\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon)",
                .name = value.name,
                .command = value.value,
                .target = shell ? target_of(value.value)
                                : userinit_target(value.value, extra.system_folder),
                .standard = standard,
                .enabled = true};
    if (!standard) {
      point.concern = Concern::winlogon_changed;
    }
    points.push_back(std::move(point));
  }
  for (const AppInit& appinit : extra.appinit) {
    const std::string location = software(appinit.wow64) + "Windows";
    const std::vector<std::string> dlls = appinit_dlls(appinit.dlls);
    if (dlls.empty()) {
      points.push_back(Point{.source = PointSource::appinit,
                             .location = location,
                             .name = "AppInit_DLLs",
                             .command = appinit.dlls,
                             .standard = true,
                             .enabled = appinit.load_enabled});
      continue;
    }
    for (const std::string& dll : dlls) {
      points.push_back(Point{.source = PointSource::appinit,
                             .location = location,
                             .name = "AppInit_DLLs",
                             .command = appinit.dlls,
                             .target = normalized(dll),
                             .enabled = appinit.load_enabled,
                             .concern = Concern::appinit_dll});
    }
  }
  return points;
}

}  // namespace myclean::security
