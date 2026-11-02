#include "core/clean/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/walk.hpp"
#include "core/clean/workspaces.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/rules/schema.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/paths.hpp"

namespace myclean::clean {

namespace {

namespace fs = std::filesystem;
using rules::CleaningRule;
using rules::Strategy;

constexpr int default_inactive_days = 14;
constexpr int default_auto_inactive_days = 30;

std::chrono::system_clock::duration days(int count) {
  return std::chrono::days{count};
}

bool removes_entries(const CleaningRule& rule) {
  return rule.strategy == Strategy::remove || rule.strategy == Strategy::quarantine;
}

bool enterable(const Entry& entry) {
  return entry.stat.kind == os::EntryKind::directory && !entry.stat.attributes.reparse_point &&
         !entry.stat.attributes.cloud_placeholder();
}

fs::path from_utf8(const std::string& text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

// Case-folded, with separators sorting before every other character, so that the
// descendants of a folder directly follow it in sorted order.
std::wstring order_key(const fs::path& path) {
  std::wstring key = path.native();
  for (wchar_t& character : key) {
    if (character >= L'A' && character <= L'Z') {
      character = static_cast<wchar_t>(character - L'A' + L'a');
    } else if (character == L'\\') {
      character = L'\x01';
    }
  }
  return key;
}

int inactive_days(const CleaningRule& rule) {
  return rule.require.inactive_days.value_or(
      rule.tier == Tier::automatic ? default_auto_inactive_days : default_inactive_days);
}

// One evaluation in progress: the candidates and drops found so far.
class Session {
 public:
  Session(const IFileTree& tree, const IUsage& usage, const safety::ProtectedLocations& locations,
          WallTime now)
      : tree_(&tree), usage_(&usage), locations_(&locations), now_(now) {}

  [[nodiscard]] WallTime now() const { return now_; }

  void drop(const CleaningRule& rule, fs::path path, std::string reason, std::string detail = {}) {
    result_.dropped.push_back(Dropped{.rule_id = rule.id,
                                      .path = std::move(path),
                                      .reason = std::move(reason),
                                      .detail = std::move(detail)});
  }

  void drop_links(const CleaningRule& rule, const Matches& matches) {
    for (const fs::path& link : matches.links) {
      drop(rule, link, "reparse_point_in_path");
    }
  }

  // Proposes `entry` for `rule` when the deny-list allows it and the rule's age and use
  // conditions hold. Processes count as using it when a folder of theirs lies in `use_root`.
  void consider(const CleaningRule& rule, const Entry& entry, const fs::path& use_root,
                const fs::path& project) {
    if (entry.stat.attributes.reparse_point || entry.stat.attributes.cloud_placeholder()) {
      return;  // a link frees nothing; a placeholder has no local content
    }
    const Result<fs::path> normal = safety::normalize_path(entry.path);
    if (!normal) {
      drop(rule, entry.path, "invalid_path", normal.error().message);
      return;
    }
    if (const std::optional<std::string_view> denied = safety::denied_by(*normal, *locations_)) {
      drop(rule, entry.path, "deny_list:" + std::string(*denied));
      return;
    }
    const Measure measured = measure(*tree_, entry);
    if (measured.size == Bytes{}) {
      return;
    }
    const bool conditional = rule.require.min_age_days || rule.require.not_in_use;
    if (conditional && !measured.complete) {
      drop(rule, entry.path, "unreadable");
      return;
    }
    if (rule.require.min_age_days && measured.newest > now_ - days(*rule.require.min_age_days)) {
      return;
    }
    if (rule.require.not_in_use && in_use(entry, use_root)) {
      drop(rule, entry.path, "in_use");
      return;
    }
    Candidate candidate = whole_rule(rule);
    candidate.path = entry.path;
    candidate.identity = entry.stat.identity;
    candidate.size = measured.size;
    candidate.newest = measured.newest;
    candidate.project = project;
    result_.candidates.push_back(std::move(candidate));
  }

  void add(Candidate candidate) { result_.candidates.push_back(std::move(candidate)); }

  // The result, with every candidate inside another one dropped (the first by rule order wins
  // for the same path).
  [[nodiscard]] Evaluation finish() && {
    std::vector<Candidate>& candidates = result_.candidates;
    std::vector<std::wstring> keys;
    keys.reserve(candidates.size());
    std::ranges::transform(candidates, std::back_inserter(keys),
                           [](const Candidate& candidate) { return order_key(candidate.path); });
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(
        order, [&keys](std::size_t one, std::size_t other) { return keys[one] < keys[other]; });
    std::vector<Candidate> kept;
    std::optional<std::size_t> container;  // index in `kept` of the last kept path candidate
    for (const std::size_t index : order) {
      Candidate& candidate = candidates[index];
      if (!candidate.path.empty() && container && within(candidate.path, kept[*container].path)) {
        result_.dropped.push_back(Dropped{.rule_id = candidate.rule_id,
                                          .path = std::move(candidate.path),
                                          .reason = "covered_by:" + kept[*container].rule_id,
                                          .detail = {}});
        continue;
      }
      if (!candidate.path.empty()) {
        container = kept.size();
      }
      kept.push_back(std::move(candidate));
    }
    candidates = std::move(kept);
    return std::move(result_);
  }

  [[nodiscard]] static Candidate whole_rule(const CleaningRule& rule) {
    return Candidate{.rule_id = rule.id,
                     .category = rule.category,
                     .tier = rule.tier,
                     .scope = rule.scope,
                     .strategy = rule.strategy,
                     .path = {},
                     .identity = {},
                     .size = {},
                     .newest = {},
                     .command = rule.command,
                     .handler = rule.handler,
                     .project = {}};
  }

 private:
  bool in_use(const Entry& entry, const fs::path& use_root) {
    if (!process_folders_) {
      process_folders_ = usage_->process_folders();
    }
    if (std::ranges::any_of(*process_folders_, [&use_root](const fs::path& folder) {
          return within(folder, use_root);
        })) {
      return true;
    }
    if (entry.stat.kind == os::EntryKind::file) {
      return usage_->locked(entry.path);
    }
    return walk(*tree_, entry.path, [this](const Entry& inner) {
             const bool file =
                 inner.stat.kind == os::EntryKind::file && !inner.stat.attributes.reparse_point;
             return file && usage_->locked(inner.path) ? Visit::stop : Visit::descend;
           }) != WalkEnd::complete;
  }

  const IFileTree* tree_;
  const IUsage* usage_;
  const safety::ProtectedLocations* locations_;
  WallTime now_;
  std::optional<std::vector<fs::path>> process_folders_;  // read once, on first need
  Evaluation result_;
};

// True when `marker` (a name pattern, or a relative path whose first component is matched
// against `listed`) exists in the folder `listed` was read from.
bool has_marker(const IFileTree& tree, const std::vector<Entry>& listed, const fs::path& marker) {
  const fs::path first = *marker.begin();
  const fs::path rest = marker.lexically_relative(first);
  return std::ranges::any_of(listed, [&](const Entry& entry) {
    if (!wildcard_match(entry.path.filename().native(), first.native())) {
      return false;
    }
    if (rest.empty() || rest == L".") {
      return true;
    }
    return enterable(entry) && !resolve(tree, entry.path, rest).entries.empty();
  });
}

// True when `entry` is a folder holding the file `name`.
bool holds_file(const IFileTree& tree, const Entry& entry, const fs::path& name) {
  if (!enterable(entry)) {
    return false;
  }
  const Result<Entry> inner = tree.stat(entry.path / name);
  return inner && inner->stat.kind == os::EntryKind::file;
}

// The folder holding the `.git` that `dir` belongs to, if any.
std::optional<fs::path> work_tree_of(const IFileTree& tree, const fs::path& dir) {
  for (fs::path folder = dir;; folder = folder.parent_path()) {
    if (tree.stat(folder / L".git")) {
      return folder;
    }
    if (folder == folder.parent_path()) {
      return std::nullopt;
    }
  }
}

bool skipped_name(const fs::path& name, std::span<const std::wstring> skip) {
  return std::ranges::any_of(skip, [&name](const std::wstring& pattern) {
    return wildcard_match(name.native(), pattern);
  });
}

struct Found {
  const CleaningRule* rule;
  Entry entry;
};

// A project root and the artifact entries its rules matched.
struct Project {
  fs::path root;
  std::vector<Found> found;
};

enum class Activity { recent, old, unknown };

// Whether a file of `project` outside its artifact folders and skipped folders changed after
// `cutoff`. `newest` receives the newest change seen when none did.
Activity source_activity(const IFileTree& tree, const Project& project,
                         std::span<const std::wstring> skip, WallTime cutoff, WallTime& newest) {
  const WalkEnd end = walk(tree, project.root, [&](const Entry& entry) {
    if (entry.stat.kind == os::EntryKind::directory) {
      const std::wstring key = order_key(entry.path);
      const bool artifact = std::ranges::any_of(
          project.found, [&key](const Found& found) { return order_key(found.entry.path) == key; });
      return artifact || skipped_name(entry.path.filename(), skip) ? Visit::skip : Visit::descend;
    }
    if (entry.touched > cutoff) {
      return Visit::stop;
    }
    newest = std::max(newest, entry.touched);
    return Visit::descend;
  });
  if (end == WalkEnd::stopped) {
    return Activity::recent;
  }
  return end == WalkEnd::complete ? Activity::old : Activity::unknown;
}

void drop_all(Session& session, const Project& project, const std::string& reason,
              const std::string& detail = {}) {
  for (const Found& found : project.found) {
    session.drop(*found.rule, found.entry.path, reason, detail);
  }
}

// The newest activity of `project` (its last commit and every file outside its artifacts) when
// nothing changed after `cutoff`; nullopt when something did, or when it cannot be told (then
// its artifacts are dropped).
std::optional<WallTime> inactive_since(Session& session, const IFileTree& tree, const IGit& git,
                                       const Project& project,
                                       const std::optional<fs::path>& work_tree,
                                       std::span<const std::wstring> skip, WallTime cutoff) {
  WallTime newest{};
  if (work_tree) {
    const Result<std::optional<WallTime>> commit = git.last_commit(*work_tree);
    if (!commit) {
      drop_all(session, project, "git_error", commit.error().message);
      return std::nullopt;
    }
    newest = commit->value_or(WallTime{});
    if (newest > cutoff) {
      return std::nullopt;
    }
  }
  const Activity activity = source_activity(tree, project, skip, cutoff, newest);
  if (activity == Activity::unknown) {
    drop_all(session, project, "unreadable");
  }
  return activity == Activity::old ? std::optional{newest} : std::nullopt;
}

// Whether git vouches for `found` inside `work_tree`: ignored and tracking nothing. Drops it with
// the reason when not.
bool git_vouches(Session& session, const IGit& git, const fs::path& work_tree, const Found& found) {
  const Result<bool> ignored = git.ignored(work_tree, found.entry.path);
  const Result<bool> tracked =
      ignored && *ignored ? git.tracks_files(work_tree, found.entry.path) : Result<bool>{false};
  if (!ignored || !tracked) {
    session.drop(*found.rule, found.entry.path, "git_error",
                 (!ignored ? ignored.error() : tracked.error()).message);
    return false;
  }
  if (!*ignored || *tracked) {
    session.drop(*found.rule, found.entry.path, *ignored ? "tracked_files" : "not_ignored");
    return false;
  }
  return true;
}

// Proposes the artifacts of `project` that pass the inactivity, git and use conditions.
void qualify(Session& session, const IFileTree& tree, const IGit& git, const Project& project,
             std::span<const std::wstring> skip) {
  const std::optional<fs::path> work_tree = work_tree_of(tree, project.root);
  const auto needs_git = [](const Found& found) {
    return found.rule->require.git_ignored && !found.rule->require.outside_git;
  };
  if (!work_tree && std::ranges::all_of(project.found, needs_git)) {
    drop_all(session, project, "not_in_git");
    return;
  }
  // A change after the shortest threshold makes the project active for every rule.
  int shortest = inactive_days(*project.found.front().rule);
  for (const Found& found : project.found) {
    shortest = std::min(shortest, inactive_days(*found.rule));
  }
  const std::optional<WallTime> newest =
      inactive_since(session, tree, git, project, work_tree, skip, session.now() - days(shortest));
  if (!newest) {
    return;
  }
  for (const Found& found : project.found) {
    const CleaningRule& rule = *found.rule;
    if (*newest > session.now() - days(inactive_days(rule))) {
      continue;
    }
    if (!work_tree && needs_git(found)) {
      session.drop(rule, found.entry.path, "not_in_git");
      continue;
    }
    if (rule.require.git_ignored && work_tree && !git_vouches(session, git, *work_tree, found)) {
      continue;
    }
    // A rule that acts inside active projects (inactive_days 0) takes a folder no build wrote
    // for its min_age_days: only a process inside that folder uses it, not one elsewhere in
    // the project (an idle terminal open in it for days).
    const fs::path& use_root = rule.require.inactive_days == 0 ? found.entry.path : project.root;
    session.consider(rule, found.entry, use_root, project.root);
  }
}

// The artifacts the rules whose marker is in `folder` (listed as `listed`) select there.
Project find_project(const IFileTree& tree, Session& session,
                     std::span<const CleaningRule* const> rules, const fs::path& folder,
                     const std::vector<Entry>& listed) {
  Project project{.root = folder, .found = {}};
  for (const CleaningRule* rule : rules) {
    if (!has_marker(tree, listed, from_utf8(*rule->project_marker))) {
      continue;
    }
    for (const std::string& target : rule->targets) {
      Matches matches = resolve(tree, folder, from_utf8(target));
      session.drop_links(*rule, matches);
      for (Entry& entry : matches.entries) {
        if (rule->require.contains &&
            !holds_file(tree, entry, from_utf8(*rule->require.contains))) {
          continue;
        }
        project.found.push_back(Found{.rule = rule, .entry = std::move(entry)});
      }
    }
  }
  return project;
}

// The folders `rule` keeps (require.outside_of, one per value of its variables); empty when it
// keeps none, nullopt when its pattern names a variable without a value.
std::optional<std::vector<fs::path>> kept_folders(const IEnvironment& environment,
                                                  const CleaningRule& rule) {
  if (!rule.require.outside_of) {
    return std::vector<fs::path>{};
  }
  const std::vector<std::wstring> values =
      expand_all(from_utf8(*rule.require.outside_of).native(), environment);
  if (values.empty()) {
    return std::nullopt;
  }
  return std::vector<fs::path>(values.begin(), values.end());
}

// True when `entry` meets require.contains and lies neither inside nor around a kept folder.
bool selectable(const IFileTree& tree, const CleaningRule& rule, std::span<const fs::path> kept,
                const Entry& entry) {
  if (rule.require.contains && !holds_file(tree, entry, from_utf8(*rule.require.contains))) {
    return false;
  }
  return std::ranges::none_of(kept, [&entry](const fs::path& folder) {
    return within(entry.path, folder) || within(folder, entry.path);
  });
}

// The entries the absolute targets of `rule` match and selectable() allows; links on the way are
// dropped. None when what the rule must keep is unknown.
std::vector<Entry> target_entries(const IFileTree& tree, const IEnvironment& environment,
                                  Session& session, const CleaningRule& rule) {
  std::vector<Entry> entries;
  const std::optional<std::vector<fs::path>> kept = kept_folders(environment, rule);
  if (!kept) {
    return entries;
  }
  for (const std::string& target : rule.targets) {
    // None when it names a variable this machine does not set; one per value of each variable.
    for (const std::wstring& expanded : expand_all(from_utf8(target).native(), environment)) {
      const fs::path pattern{expanded};
      if (!on_every_volume(pattern) &&
          (!pattern.has_root_name() || !pattern.has_root_directory())) {
        session.drop(rule, pattern, "invalid_path", "a target must be an absolute path");
        continue;
      }
      Matches matches = resolve(tree, pattern);
      session.drop_links(rule, matches);
      std::ranges::copy_if(matches.entries, std::back_inserter(entries), [&](const Entry& entry) {
        return selectable(tree, rule, *kept, entry);
      });
    }
  }
  return entries;
}

}  // namespace

RuleEngine::RuleEngine(const IFileTree& tree, const IEnvironment& environment, const IUsage& usage,
                       safety::ProtectedLocations locations)
    : tree_(&tree), environment_(&environment), usage_(&usage), locations_(std::move(locations)) {}

Evaluation RuleEngine::evaluate(std::span<const CleaningRule> rules, WallTime now) const {
  Session session(*tree_, *usage_, locations_, now);
  for (const CleaningRule& rule : rules) {
    if (rule.project_marker) {
      continue;
    }
    const std::vector<Entry> entries = target_entries(*tree_, *environment_, session, rule);
    if (removes_entries(rule)) {
      for (const Entry& entry : entries) {
        session.consider(rule, entry, entry.path, {});
      }
      continue;
    }
    Candidate whole = Session::whole_rule(rule);
    for (const Entry& entry : entries) {
      if (!entry.stat.attributes.reparse_point) {
        whole.size += measure(*tree_, entry).size;
      }
    }
    if (rule.targets.empty() || whole.size > Bytes{}) {
      session.add(std::move(whole));
    }
  }
  return std::move(session).finish();
}

Evaluation RuleEngine::scan_artifacts(std::span<const CleaningRule> rules,
                                      const Workspaces& workspaces, const IGit& git,
                                      WallTime now) const {
  Session session(*tree_, *usage_, locations_, now);
  std::vector<const CleaningRule*> project_rules;
  for (const CleaningRule& rule : rules) {
    if (rule.project_marker && removes_entries(rule)) {
      project_rules.push_back(&rule);
    }
  }
  std::set<std::wstring> artifacts;  // order_key of every artifact entry found so far
  for (const fs::path& root : workspaces.roots) {
    std::vector<std::pair<fs::path, int>> pending = {{root, 0}};
    while (!pending.empty()) {
      const auto [folder, depth] = std::move(pending.back());
      pending.pop_back();
      const Result<std::vector<Entry>> listed = tree_->list(folder);
      if (!listed) {
        continue;
      }
      const Project project = find_project(*tree_, session, project_rules, folder, *listed);
      for (const Found& found : project.found) {
        artifacts.insert(order_key(found.entry.path));
      }
      for (const Entry& entry : *listed) {
        if (depth < workspaces.max_depth && enterable(entry) &&
            !skipped_name(entry.path.filename(), workspaces.skip) &&
            !artifacts.contains(order_key(entry.path))) {
          pending.emplace_back(entry.path, depth + 1);
        }
      }
      if (!project.found.empty()) {
        qualify(session, *tree_, git, project, workspaces.skip);
      }
    }
  }
  return std::move(session).finish();
}

}  // namespace myclean::clean
