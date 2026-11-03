#include "core/governor/orphans.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"

namespace myclean::governor {

namespace {

constexpr std::array<std::string_view, 2> rule_names = {"always", "ignore"};
constexpr std::array<std::string_view, 4> decision_names = {"end_now", "always", "ignore",
                                                            "forget"};

constexpr std::array<std::string_view, 22> loop_images = {
    "bash", "sh",   "dash",  "zsh",     "fish",  "ksh",        "pwsh", "powershell",
    "cmd",  "py",   "pyw",   "node",    "deno",  "bun",        "perl", "ruby",
    "php",  "tail", "sleep", "timeout", "watch", "inotifywait"};
constexpr std::array<std::string_view, 2> script_hosts = {"wscript", "cscript"};

constexpr std::size_t min_hex_run = 8;

char lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string lowered(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), lower);
  return out;
}

// "C:\Tools\Bash.EXE" -> "bash".
std::string program_name(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  std::string name = lowered(slash == std::string_view::npos ? path : path.substr(slash + 1));
  if (name.ends_with(".exe")) {
    name.resize(name.size() - 4);
  }
  return name;
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

bool is_hex_or_dash(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || c == '-';
}

// A number, or a run of hex digits and dashes long enough to be an id, with a digit in it.
bool variable(std::string_view segment) {
  if (segment.empty()) {
    return false;
  }
  if (std::ranges::all_of(segment, is_digit)) {
    return true;
  }
  return segment.size() >= min_hex_run && std::ranges::all_of(segment, is_hex_or_dash) &&
         std::ranges::any_of(segment, is_digit);
}

bool separator(char c) {
  return c == '/' || c == '\\' || c == '=' || c == ':' || c == ',' || c == '.';
}

// One argument with its variable parts as '#'.
std::string normalize_argument(std::string_view argument) {
  std::string out;
  std::string segment;
  const auto flush = [&] {
    out += variable(segment) ? std::string("#") : segment;
    segment.clear();
  };
  for (const char c : argument) {
    if (separator(c)) {
      flush();
      out += c;
    } else {
      segment += lower(c);
    }
  }
  flush();
  return out;
}

// The arguments of a command line: split at white space outside double quotes, quotes dropped.
std::vector<std::string> arguments(std::string_view command_line) {
  std::vector<std::string> parts;
  std::string current;
  bool quoted = false;
  bool any = false;
  for (const char c : command_line) {
    if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
      if (any) {
        parts.push_back(current);
      }
      current.clear();
      any = false;
    } else {
      current += c;
      any = true;
    }
  }
  if (any) {
    parts.push_back(current);
  }
  return parts;
}

}  // namespace

std::string_view to_string(OrphanRule rule) {
  return detail::enum_name(rule_names, rule);
}

std::optional<OrphanRule> parse_orphan_rule(std::string_view text) {
  return detail::enum_from_name<OrphanRule>(rule_names, text);
}

std::optional<OrphanDecision> parse_orphan_decision(std::string_view text) {
  return detail::enum_from_name<OrphanDecision>(decision_names, text);
}

bool loop_image(std::string_view image) {
  const std::string name = program_name(image);
  const auto listed = [&](std::string_view entry) { return entry == name; };
  if (std::ranges::any_of(loop_images, listed) || std::ranges::any_of(script_hosts, listed)) {
    return true;
  }
  // python, python3, python3.12, pythonw
  if (!name.starts_with("python")) {
    return false;
  }
  const std::string_view rest = std::string_view(name).substr(6);
  return std::ranges::all_of(rest, [](char c) { return is_digit(c) || c == '.' || c == 'w'; });
}

std::string normalize_command_line(std::string_view command_line) {
  const std::vector<std::string> parts = arguments(command_line);
  std::string pattern;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index > 0) {
      pattern += ' ';
    }
    pattern += index == 0 ? program_name(parts[index]) : normalize_argument(parts[index]);
  }
  if (pattern.size() > max_pattern_length) {
    pattern.resize(max_pattern_length);
  }
  return pattern;
}

std::vector<std::size_t> orphan_roots(std::span<const OrphanProcess> processes, WallTime now,
                                      const OrphanPolicy& policy) {
  std::vector<std::size_t> roots;
  if (!policy.enabled) {
    return roots;
  }
  std::unordered_map<std::uint32_t, WallTime> created;
  created.reserve(processes.size());
  for (const OrphanProcess& process : processes) {
    created.emplace(process.key.pid, process.key.created);
  }
  for (std::size_t index = 0; index < processes.size(); ++index) {
    const OrphanProcess& process = processes[index];
    if (process.protected_process || !loop_image(process.image) ||
        now - process.key.created < policy.min_age) {
      continue;
    }
    const auto parent = created.find(process.parent_pid);
    const bool parent_alive = parent != created.end() && process.parent_pid != process.key.pid &&
                              parent->second < process.key.created;
    if (!parent_alive) {
      roots.push_back(index);
    }
  }
  return roots;
}

std::vector<OrphanGroup> group_orphans(std::span<const OrphanProcess> processes,
                                       std::span<const std::size_t> roots) {
  std::vector<std::vector<std::size_t>> children(processes.size());
  for (std::size_t index = 0; index < processes.size(); ++index) {
    const std::optional<std::size_t> parent = processes[index].parent;
    // Only a parent created before its child: links then never form a cycle.
    if (parent && *parent < processes.size() &&
        processes[*parent].key.created < processes[index].key.created) {
      children[*parent].push_back(index);
    }
  }
  std::vector<OrphanGroup> groups;
  std::map<std::string, std::size_t, std::less<>> by_pattern;
  for (const std::size_t root : roots) {
    const OrphanProcess& process = processes[root];
    if (!process.command_line || process.protected_process) {
      continue;
    }
    OrphanTree tree{.root = root, .members = {root}};
    // Breadth first; each process has one parent, so none is reached twice.
    for (std::size_t next = 0; next < tree.members.size(); ++next) {
      for (const std::size_t child : children[tree.members[next]]) {
        if (!processes[child].protected_process) {
          tree.members.push_back(child);
        }
      }
    }
    std::string pattern = normalize_command_line(*process.command_line);
    const auto [found, added] = by_pattern.try_emplace(pattern, groups.size());
    if (added) {
      groups.push_back(OrphanGroup{.pattern = std::move(pattern), .image = process.image});
    }
    groups[found->second].trees.push_back(std::move(tree));
  }
  return groups;
}

}  // namespace myclean::governor
