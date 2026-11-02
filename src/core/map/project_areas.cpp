#include "core/map/project_areas.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/map/attribution.hpp"
#include "core/rules/schema.hpp"
#include "core/space/scanner.hpp"

namespace myclean::map {

namespace {

namespace fs = std::filesystem;

fs::path from_utf8(const std::string& text) {
  return fs::path{std::u8string(text.begin(), text.end())};
}

// The components of a relative pattern ("target\*\incremental\*").
std::vector<std::wstring> components_of(const std::string& pattern) {
  std::vector<std::wstring> parts;
  for (const fs::path& part : from_utf8(pattern)) {
    if (!part.empty() && part != L".") {
      parts.push_back(part.native());
    }
  }
  return parts;
}

bool matches(const fs::path& name, const std::wstring& pattern) {
  return clean::wildcard_match(name.native(), pattern);
}

// What `parts` names below the scan folder `index`: the folders, and with `files` the files too.
// Returns folder indices; `file_hit` is set when a file matches.
std::vector<std::size_t> resolve(const space::ScanResult& scan, std::size_t index,
                                 std::span<const std::wstring> parts, bool* file_hit = nullptr) {
  if (parts.empty()) {
    return {};
  }
  std::vector<std::size_t> frontier = {index};
  for (const std::wstring& part : parts.first(parts.size() - 1)) {
    std::vector<std::size_t> next;
    for (const std::size_t folder : frontier) {
      std::ranges::copy_if(
          scan.folders[folder].folders, std::back_inserter(next),
          [&](std::size_t child) { return matches(scan.folders[child].path.filename(), part); });
    }
    frontier = std::move(next);
  }
  std::vector<std::size_t> hits;
  for (const std::size_t folder : frontier) {
    std::ranges::copy_if(scan.folders[folder].folders, std::back_inserter(hits),
                         [&](std::size_t child) {
                           return matches(scan.folders[child].path.filename(), parts.back());
                         });
    if (file_hit != nullptr &&
        std::ranges::any_of(scan.folders[folder].files, [&](const space::FileItem& file) {
          return !file.link && matches(file.name, parts.back());
        })) {
      *file_hit = true;
    }
  }
  return hits;
}

bool has_marker(const space::ScanResult& scan, std::size_t index, const std::string& marker) {
  bool file_hit = false;
  return !resolve(scan, index, components_of(marker), &file_hit).empty() || file_hit;
}

bool holds_file(const space::ScanResult& scan, std::size_t folder, const std::wstring& name) {
  return std::ranges::any_of(scan.folders[folder].files, [&name](const space::FileItem& file) {
    return !file.link && matches(file.name, name);
  });
}

bool skipped(const fs::path& name, std::span<const std::wstring> skip) {
  return std::ranges::any_of(
      skip, [&name](const std::wstring& pattern) { return matches(name, pattern); });
}

// Adds to `paths` the artifact folders `rule` selects in the project at folder `index`, and
// closes them. Artifacts are folders: a file a pattern names (build.zig for build*) is not one.
void add_artifacts(const space::ScanResult& scan, std::size_t index,
                   const rules::CleaningRule& rule, std::vector<std::wstring>& paths,
                   std::vector<bool>& closed) {
  const std::optional<std::wstring> contains =
      rule.require.contains ? std::optional{from_utf8(*rule.require.contains).native()}
                            : std::nullopt;
  for (const std::string& target : rule.targets) {
    for (const std::size_t folder : resolve(scan, index, components_of(target))) {
      if (contains && !holds_file(scan, folder, *contains)) {
        continue;
      }
      closed[folder] = true;
      const std::wstring& path = scan.folders[folder].path.native();
      if (path.find(L'%') == std::wstring::npos) {
        paths.push_back(path);
      }
    }
  }
}

Area area_of(const rules::CleaningRule& rule, std::vector<std::wstring> paths) {
  return Area{.id = rule.id,
              .paths = std::move(paths),
              .category = Category::build_artifacts,
              .label = "Output of projects with " + *rule.project_marker,
              .reclaim = Reclaim{.kind = ReclaimKind::clean_rule,
                                 .ref = rule.id,
                                 .note = "proposed once git ignores it, no process uses it and "
                                         "the project is idle long enough"}};
}

}  // namespace

std::vector<Area> project_areas(const space::ScanResult& scan,
                                std::span<const rules::CleaningRule> rules,
                                std::span<const std::wstring> skip) {
  std::vector<std::vector<std::wstring>> paths(rules.size());
  // Set for an artifact folder, a skipped one and everything below them.
  std::vector<bool> closed(scan.folders.size(), false);
  for (std::size_t index = 0; index < scan.folders.size(); ++index) {
    const space::Folder& folder = scan.folders[index];
    if (folder.parent != space::no_parent &&
        (closed[folder.parent] || skipped(folder.path.filename(), skip))) {
      closed[index] = true;
    }
    if (closed[index]) {
      continue;
    }
    for (std::size_t rule = 0; rule < rules.size(); ++rule) {
      if (rules[rule].project_marker && has_marker(scan, index, *rules[rule].project_marker)) {
        add_artifacts(scan, index, rules[rule], paths[rule], closed);
      }
    }
  }
  std::vector<Area> areas;
  for (std::size_t rule = 0; rule < rules.size(); ++rule) {
    if (!paths[rule].empty()) {
      areas.push_back(area_of(rules[rule], std::move(paths[rule])));
    }
  }
  return areas;
}

}  // namespace myclean::map
