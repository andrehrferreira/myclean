#pragma once

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/action.hpp"
#include "core/model/units.hpp"

// The data that drives cleaning, process classification and the process knowledge base, as
// loaded from data/rules/*.toml, data/classes.toml and data/kb/*.json (architecture F-042).
// The file formats are documented next to each type; parse.hpp loads them.
namespace myclean::rules {

// A regular expression from a data file: ECMAScript syntax, case-insensitive. `text` is the
// source as written, for display; `regex` is compiled at load, so a malformed one rejects the
// file.
struct Pattern {
  std::string text;
  std::regex regex;
};

// ---- data/rules/*.toml: cleaning rules ------------------------------------------------------
//
//   [[rule]]
//   id       = "dev.node.node_modules"        # required, unique across all rule files
//   category = "dev-artifacts"                # required
//   tier     = "auto"                         # required: auto | ask | report
//   scope    = "user"                         # required: user (tray, owner token) | system (svc)
//   strategy = "delete"                       # required: delete | quarantine | native | handler
//   targets  = ["node_modules", ".next"]      # required for delete and quarantine
//   command  = "pnpm store prune"             # required for native, not allowed otherwise
//   handler  = "Temporary Files"              # required for handler, not allowed otherwise
//   project  = { marker = "package.json" }    # optional: targets are relative to a project root
//   require  = { git_ignored = true, inactive_days = 30, not_in_use = true, min_age_days = 2 }

// How a rule removes what it selects.
enum class Strategy {
  remove,      // text form "delete": deleted through SafeRemove
  quarantine,  // moved into the volume's quarantine through SafeRemove
  native,      // the tool's own prune command (CleaningRule::command)
  handler,     // a Windows disk-cleanup handler (CleaningRule::handler)
};

[[nodiscard]] std::string_view to_string(Strategy strategy);
[[nodiscard]] std::optional<Strategy> parse_strategy(std::string_view text);

// Conditions every selected item must meet. Absent values impose no condition.
struct Requirements {
  bool git_ignored = false;          // `git check-ignore` reports the item as ignored
  bool not_in_use = false;           // no process has its current directory or a file open in it
  std::optional<int> inactive_days;  // project untouched (last commit, newest source) this long
  std::optional<int> min_age_days;   // the item itself is at least this old
};

struct CleaningRule {
  std::string id;
  std::string category;
  Tier tier = Tier::ask;
  ExecutionContext scope = ExecutionContext::user;  // "system" in the file is `service`
  Strategy strategy = Strategy::quarantine;
  std::vector<std::string> targets;
  std::string command;  // native only
  std::string handler;  // handler only
  std::optional<std::string> project_marker;
  Requirements require;
};

// ---- data/classes.toml: governor process classes --------------------------------------------
//
//   [[match]]                                 # ordered: the first matching entry wins
//   id           = "build.node.vite"          # required, unique
//   class        = "build"                    # required: system | game | allow | build | test |
//                                             #           default
//   image        = ["node.exe"]               # image names; wildcards are the matcher's concern
//   command_line = "\\bvite(\\.js)?\\b"       # Pattern
//   path_root    = ["E:\\Games"]              # image path prefixes
//   signer       = ["Microsoft Corporation"]
//
//   [caps.build]                              # optional; only build and test have job caps
//   cpu_percent = 75                          # 1..100
//   commit_gib  = 96                          # >= 1
//
// An entry sets at least one of image, command_line, path_root and signer. It matches a process
// when every criterion it sets matches; a list matches when any of its elements does.

enum class ProcessClass {
  system,
  game,
  allow,
  build,
  test,
  normal,  // text form "default": every process no entry matches
};

[[nodiscard]] std::string_view to_string(ProcessClass process_class);
[[nodiscard]] std::optional<ProcessClass> parse_process_class(std::string_view text);

struct ClassMatch {
  std::string id;
  ProcessClass process_class = ProcessClass::normal;
  std::vector<std::string> images;  // empty: not a criterion
  std::optional<Pattern> command_line;
  std::vector<std::string> path_roots;  // empty: not a criterion
  std::vector<std::string> signers;     // empty: not a criterion
};

// Hard caps of a class's job object (governor spec, "Proactive Build and Test Jobs").
struct JobCaps {
  int cpu_percent = 75;
  Bytes commit;
};

struct ClassSet {
  std::vector<ClassMatch> matches;  // in file order
  JobCaps build{.cpu_percent = 75, .commit = Bytes{96ULL << 30U}};
  JobCaps test{.cpu_percent = 75, .commit = Bytes{64ULL << 30U}};
};

// ---- data/kb/*.json: process and app knowledge base -----------------------------------------
//
//   { "entries": [ {
//       "id": "microsoft.powertoys.keyboard-manager",   required, unique across all kb files
//       "image": "PowerToys.KeyboardManagerEngine.exe", required
//       "signer": "Microsoft Corporation",              optional
//       "path": "\\\\PowerToys\\\\",                    optional Pattern over the image path
//       "description": "Remaps keys and shortcuts.",    required, plain language
//       "category": "utility",                          required
//       "needed_at_startup": false,                     required
//       "safe_to_remove": true                          required
//   } ] }

struct KbEntry {
  std::string id;
  std::string image;
  std::optional<std::string> signer;
  std::optional<Pattern> path;
  std::string description;
  std::string category;
  bool needed_at_startup = false;
  bool safe_to_remove = false;
};

}  // namespace myclean::rules
