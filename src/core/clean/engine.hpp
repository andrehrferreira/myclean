#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "core/clean/probes.hpp"
#include "core/clean/workspaces.hpp"
#include "core/model/action.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"

// The cleaning rule engine: evaluates the loaded rules against the machine and lists what they
// select. It only reads; acting on a candidate is the executor's job, through SafeRemove (delete
// and quarantine), the tray (native commands) or the service (Windows handlers).
namespace myclean::clean {

// One thing a rule selects.
struct Candidate {
  std::string rule_id;
  std::string category;
  Tier tier = Tier::ask;
  ExecutionContext scope = ExecutionContext::user;
  rules::Strategy strategy = rules::Strategy::remove;
  // delete and quarantine: the entry to remove, with its identity when the scan saw it. Empty
  // for native and handler rules, which act on the whole rule.
  std::filesystem::path path;
  FileIdentity identity;
  // Size on disk. native and handler: what their measured targets hold now, an estimate of
  // what the command or handler frees (zero when the rule names no target; a handler reports
  // its own size).
  Bytes size;
  WallTime newest;      // the newest change inside `path`; empty for native and handler
  std::string command;  // native
  std::string handler;  // handler
  // Developer artifacts: the project root whose marker matched; empty otherwise.
  std::filesystem::path project;
};

// An entry a rule matched that is not proposed, for the log. Entries that are simply too
// recent, or projects still active, are not listed. Reasons are fixed codes:
//   invalid_path            not an absolute drive path
//   deny_list:<entry>       SafeRemove's deny-list refuses it (safety::deny_list())
//   reparse_point_in_path   the target runs through a junction or link
//   in_use                  a process has a file open in it, its current directory or its image
//                           inside it (or, for an artifact, inside its project)
//   unreadable              part of it cannot be listed, so its age or use cannot be proven
//   not_in_git              its rule requires git_ignored without outside_git, but the project
//                           is in no git work tree
//   not_ignored             inside a git work tree, `git check-ignore` does not ignore it
//   tracked_files           git tracks files inside it
//   git_error               git failed; `detail` has the message
//   covered_by:<rule id>    another candidate already contains it
struct Dropped {
  std::string rule_id;
  std::filesystem::path path;
  std::string reason;
  std::string detail;
};

struct Evaluation {
  std::vector<Candidate> candidates;  // no candidate path lies inside another one
  std::vector<Dropped> dropped;
};

class RuleEngine {
 public:
  // `locations` are the deny-list locations SafeRemove will apply at execution time; every
  // candidate it would refuse is dropped here already.
  RuleEngine(const IFileTree& tree, const IEnvironment& environment, const IUsage& usage,
             safety::ProtectedLocations locations);

  // The rules without a project marker. A target is an absolute path pattern: %VARIABLES% are
  // expanded (a target with an unset variable is skipped), a "?:\" drive stands for every fixed
  // volume (IFileTree::fixed_volumes), '*' and '?' match within one component, and every entry
  // the last component matches is one candidate. Conditions:
  //   min_age_days  the entry and everything inside it changed at least that long ago
  //   not_in_use    no file inside it is locked, no process folder lies inside it
  // Native and handler rules give one candidate each, sized by their targets; a rule whose
  // targets hold nothing (the tool's cache is absent or empty here) gives none.
  [[nodiscard]] Evaluation evaluate(std::span<const rules::CleaningRule> rules, WallTime now) const;

  // The rules with a project marker, over the workspace roots (developer-artifact spec,
  // analysis F-012). A folder is a project root of a rule when the rule's marker (a name
  // pattern, or a relative path such as "ProjectSettings\ProjectVersion.txt") exists in it;
  // the rule's targets are patterns relative to that root. Folders named in
  // `workspaces.skip`, artifact folders, links and cloud placeholders are never entered. An
  // artifact is proposed only when
  //   - the project is inactive: the newest of HEAD's commit time and the change time of
  //     every file outside its artifact folders is at least require.inactive_days old (by
  //     default 30 for auto-tier rules, 14 otherwise);
  //   - with require.git_ignored: the project is inside a git work tree, and git ignores the
  //     artifact and tracks no file inside it. Outside a work tree nothing vouches that the
  //     folder is regenerable (an engine's ThirdParty "build" or a tool's "bin" look the same),
  //     so it is never proposed, unless the rule also sets require.outside_git (its targets
  //     are written only by a tool): then a project outside any work tree qualifies on the
  //     other conditions, and inside one git must still vouch;
  //   - with require.not_in_use: no locked file inside it and no process folder inside the
  //     project; for a rule with require.inactive_days = 0 (it acts inside active projects),
  //     no process folder inside the artifact itself.
  [[nodiscard]] Evaluation scan_artifacts(std::span<const rules::CleaningRule> rules,
                                          const Workspaces& workspaces, const IGit& git,
                                          WallTime now) const;

 private:
  const IFileTree* tree_;
  const IEnvironment* environment_;
  const IUsage* usage_;
  safety::ProtectedLocations locations_;
};

}  // namespace myclean::clean
