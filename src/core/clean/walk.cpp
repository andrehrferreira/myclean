#include "core/clean/walk.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/model/result.hpp"
#include "core/os/file_system.hpp"

namespace myclean::clean {

namespace {

namespace fs = std::filesystem;

bool enterable(const Entry& entry) {
  return entry.stat.kind == os::EntryKind::directory && !entry.stat.attributes.reparse_point &&
         !entry.stat.attributes.cloud_placeholder();
}

// The entries of `dir` named by `component`: a listing for a wildcard, one stat otherwise.
std::vector<Entry> children(const IFileTree& tree, const fs::path& dir,
                            const std::wstring& component) {
  if (!has_wildcard(component)) {
    Result<Entry> entry = tree.stat(dir / component);
    return entry ? std::vector<Entry>{std::move(*entry)} : std::vector<Entry>{};
  }
  Result<std::vector<Entry>> listed = tree.list(dir);
  if (!listed) {
    return {};
  }
  std::erase_if(*listed, [&component](const Entry& entry) {
    return !wildcard_match(entry.path.filename().native(), component);
  });
  return std::move(*listed);
}

}  // namespace

Matches resolve(const IFileTree& tree, const fs::path& base, const fs::path& pattern) {
  std::vector<std::wstring> components;
  for (const fs::path& component : pattern) {
    if (component == L"..") {
      return {};
    }
    if (!component.empty() && component != L"." && component.native() != L"\\") {
      components.push_back(component.native());
    }
  }
  Matches matches;
  if (components.empty()) {
    return matches;
  }
  std::vector<fs::path> folders = {base};
  for (std::size_t index = 0; index + 1 < components.size(); ++index) {
    std::vector<fs::path> next;
    for (const fs::path& folder : folders) {
      for (const Entry& entry : children(tree, folder, components[index])) {
        if (entry.stat.attributes.reparse_point) {
          matches.links.push_back(entry.path);
        } else if (enterable(entry)) {
          next.push_back(entry.path);
        }
      }
    }
    folders = std::move(next);
  }
  for (const fs::path& folder : folders) {
    std::ranges::move(children(tree, folder, components.back()),
                      std::back_inserter(matches.entries));
  }
  return matches;
}

Matches resolve(const IFileTree& tree, const fs::path& pattern) {
  return resolve(tree, pattern.root_path(), pattern.relative_path());
}

WalkEnd walk(const IFileTree& tree, const fs::path& root,
             const std::function<Visit(const Entry&)>& visit) {
  WalkEnd end = WalkEnd::complete;
  std::vector<fs::path> pending = {root};
  while (!pending.empty()) {
    const fs::path folder = std::move(pending.back());
    pending.pop_back();
    const Result<std::vector<Entry>> listed = tree.list(folder);
    if (!listed) {
      if (listed.error().code != Errc::not_found) {
        end = WalkEnd::incomplete;
      }
      continue;
    }
    for (const Entry& entry : *listed) {
      const Visit next = visit(entry);
      if (next == Visit::stop) {
        return WalkEnd::stopped;
      }
      if (next == Visit::descend && enterable(entry)) {
        pending.push_back(entry.path);
      }
    }
  }
  return end;
}

Measure measure(const IFileTree& tree, const Entry& entry) {
  Measure result{.size = entry.stat.size_on_disk, .newest = entry.touched};
  if (!enterable(entry)) {
    return result;
  }
  result.complete = walk(tree, entry.path, [&result](const Entry& inner) {
                      result.size += inner.stat.size_on_disk;
                      result.newest = std::max(result.newest, inner.touched);
                      return Visit::descend;
                    }) == WalkEnd::complete;
  return result;
}

}  // namespace myclean::clean
