#include "core/indexer/folders.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/walk.hpp"
#include "core/clean/workspaces.hpp"
#include "core/indexer/scope.hpp"
#include "core/map/attribution.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"
#include "core/rules/schema.hpp"

namespace myclean::indexer {
namespace {

namespace fs = std::filesystem;

fs::path from_utf8(const std::string& text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

// A folder that may be entered or listed: not a link, not a cloud placeholder.
bool plain_folder(const os::FileStat& stat) {
  return stat.kind == os::EntryKind::directory && !stat.attributes.reparse_point &&
         !stat.attributes.cloud_placeholder();
}

bool skipped(const fs::path& name, std::span<const std::wstring> skip) {
  return std::ranges::any_of(skip, [&name](const std::wstring& pattern) {
    return clean::wildcard_match(name.native(), pattern);
  });
}

// The plain folders `pattern` names below `base` (an absolute pattern when `base` is empty).
std::vector<fs::path> folders_of(const clean::IFileTree& tree, const fs::path& base,
                                 const fs::path& pattern) {
  const clean::Matches matches =
      base.empty() ? clean::resolve(tree, pattern) : clean::resolve(tree, base, pattern);
  std::vector<fs::path> folders;
  for (const clean::Entry& entry : matches.entries) {
    if (plain_folder(entry.stat)) {
      folders.push_back(entry.path);
    }
  }
  return folders;
}

void add_areas(const clean::IFileTree& tree, const clean::IEnvironment& environment,
               std::span<const map::Area> areas, std::vector<IndexedFolder>& found) {
  for (const map::Area& area : areas) {
    if (!kept_out(area.category)) {
      continue;
    }
    for (const std::wstring& path : area.paths) {
      for (const std::wstring& expanded : clean::expand_all(path, environment)) {
        for (fs::path& folder : folders_of(tree, {}, fs::path{expanded})) {
          found.push_back(IndexedFolder{.path = std::move(folder),
                                        .category = std::string(map::to_string(area.category)),
                                        .label = area.label});
        }
      }
    }
  }
}

// Whether `entries` (the listing of `folder`) hold the marker of `rule`.
bool has_marker(const clean::IFileTree& tree, const fs::path& folder,
                std::span<const clean::Entry> entries, const rules::CleaningRule& rule) {
  const fs::path marker = from_utf8(*rule.project_marker);
  if (std::distance(marker.begin(), marker.end()) > 1) {
    return !clean::resolve(tree, folder, marker).entries.empty();
  }
  return std::ranges::any_of(entries, [&marker](const clean::Entry& entry) {
    return clean::wildcard_match(entry.path.filename().native(), marker.native());
  });
}

// Adds to `found` and `artifacts` the folders `rule`'s targets select in the project at
// `folder`.
void add_artifacts(const clean::IFileTree& tree, const fs::path& folder,
                   const rules::CleaningRule& rule, std::set<fs::path>& artifacts,
                   std::vector<IndexedFolder>& found) {
  for (const std::string& target : rule.targets) {
    for (fs::path& artifact : folders_of(tree, folder, from_utf8(target))) {
      if (rule.require.contains &&
          clean::resolve(tree, artifact, from_utf8(*rule.require.contains)).entries.empty()) {
        continue;
      }
      artifacts.insert(artifact);
      found.push_back(
          IndexedFolder{.path = std::move(artifact),
                        .category = std::string(map::to_string(map::Category::build_artifacts)),
                        .label = "Output of projects with " + *rule.project_marker});
    }
  }
}

void add_projects(const clean::IFileTree& tree, const Knowledge& knowledge,
                  std::vector<IndexedFolder>& found, const std::stop_token& stop) {
  std::vector<const rules::CleaningRule*> projects;
  for (const rules::CleaningRule& rule : knowledge.rules) {
    if (rule.project_marker) {
      projects.push_back(&rule);
    }
  }
  if (knowledge.workspaces == nullptr || projects.empty()) {
    return;
  }
  const clean::Workspaces& workspaces = *knowledge.workspaces;
  std::set<fs::path> artifacts;  // found in projects: never entered
  std::vector<std::pair<fs::path, int>> pending;
  pending.reserve(workspaces.roots.size());
  for (const fs::path& root : workspaces.roots) {
    pending.emplace_back(root, 0);
  }
  while (!pending.empty() && !stop.stop_requested()) {
    auto [folder, depth] = std::move(pending.back());
    pending.pop_back();
    Result<std::vector<clean::Entry>> entries = tree.list(folder);
    if (!entries) {
      continue;  // gone or unreadable: nothing below it can be judged
    }
    for (const rules::CleaningRule* rule : projects) {
      if (has_marker(tree, folder, *entries, *rule)) {
        add_artifacts(tree, folder, *rule, artifacts, found);
      }
    }
    if (depth >= workspaces.max_depth) {
      continue;
    }
    for (const clean::Entry& entry : *entries) {
      if (plain_folder(entry.stat) && !skipped(entry.path.filename(), workspaces.skip) &&
          !artifacts.contains(entry.path)) {
        pending.emplace_back(entry.path, depth + 1);
      }
    }
  }
}

}  // namespace

bool kept_out(map::Category category) {
  return category == map::Category::build_artifacts || category == map::Category::dev_caches ||
         category == map::Category::app_caches;
}

Result<std::vector<IndexedFolder>> indexed_build_folders(const IIndexerScope& scope,
                                                         const clean::IFileTree& tree,
                                                         const clean::IEnvironment& environment,
                                                         const Knowledge& knowledge,
                                                         const std::stop_token& stop) {
  std::vector<IndexedFolder> found;
  add_areas(tree, environment, knowledge.areas, found);
  add_projects(tree, knowledge, found, stop);
  if (stop.stop_requested()) {
    return fail(Errc::invalid_state, "the indexer-scope check was stopped");
  }
  std::ranges::sort(found, {}, &IndexedFolder::path);
  std::vector<IndexedFolder> indexed;
  for (IndexedFolder& folder : found) {
    if (!indexed.empty() && clean::within(folder.path, indexed.back().path)) {
      continue;  // the same folder again, or one inside a folder found
    }
    Result<bool> included = scope.included(folder.path);
    if (!included) {
      return tl::unexpected(included.error());
    }
    if (*included) {
      indexed.push_back(std::move(folder));
    }
  }
  return indexed;
}

}  // namespace myclean::indexer
