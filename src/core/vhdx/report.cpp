#include "core/vhdx/report.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/clean/patterns.hpp"
#include "core/clean/probes.hpp"
#include "core/clean/walk.hpp"
#include "core/map/attribution.hpp"
#include "core/model/text.hpp"

namespace myclean::vhdx {

namespace {

namespace fs = std::filesystem;

// Folder levels below a source folder searched for disks: Docker Desktop keeps
// <folder>\disk\docker_data.vhdx and <folder>\main\ext4.vhdx.
constexpr std::size_t max_depth = 2;

bool readable_file(const clean::Entry& entry) {
  return entry.stat.kind == os::EntryKind::file && !entry.stat.attributes.reparse_point &&
         !entry.stat.attributes.cloud_placeholder();
}

std::size_t depth_below(const fs::path& path, const fs::path& root) {
  const fs::path relative = path.lexically_relative(root);
  return static_cast<std::size_t>(std::distance(relative.begin(), relative.end()));
}

// The .vhdx entries of `source`: the file itself, or those up to max_depth levels below it.
std::vector<clean::Entry> disks_of(const clean::IFileTree& tree, const DiskSource& source) {
  const Result<clean::Entry> self = tree.stat(source.location);
  if (!self || self->stat.attributes.reparse_point || self->stat.attributes.cloud_placeholder()) {
    return {};
  }
  if (self->stat.kind == os::EntryKind::file) {
    return is_vhdx(self->path) ? std::vector<clean::Entry>{*self} : std::vector<clean::Entry>{};
  }
  std::vector<clean::Entry> found;
  static_cast<void>(clean::walk(tree, source.location, [&](const clean::Entry& entry) {
    const std::size_t depth = depth_below(entry.path, source.location);
    if (readable_file(entry) && is_vhdx(entry.path)) {
      found.push_back(entry);
    }
    return depth < max_depth ? clean::Visit::descend : clean::Visit::skip;
  }));
  return found;
}

}  // namespace

bool is_vhdx(const fs::path& path) {
  return clean::wildcard_match(path.filename().native(), L"*.vhdx");
}

std::vector<DiskSource> area_sources(const clean::IFileTree& tree, std::span<const map::Area> areas,
                                     const clean::IEnvironment& environment) {
  std::vector<DiskSource> sources;
  for (const map::Area& area : areas) {
    if (area.category != map::Category::vm_disks) {
      continue;
    }
    for (const std::wstring& pattern : area.paths) {
      for (const std::wstring& expanded : clean::expand_all(pattern, environment)) {
        for (const clean::Entry& entry : clean::resolve(tree, fs::path{expanded}).entries) {
          sources.push_back(
              DiskSource{.location = entry.path, .area = area.id, .label = area.label});
        }
      }
    }
  }
  return sources;
}

std::vector<VirtualDisk> find_disks(const clean::IFileTree& tree,
                                    std::span<const DiskSource> sources,
                                    const clean::IUsage* usage) {
  std::vector<VirtualDisk> disks;
  std::vector<FileIdentity> seen;
  std::set<std::wstring> seen_paths;
  for (const DiskSource& source : sources) {
    for (const clean::Entry& entry : disks_of(tree, source)) {
      const bool known = entry.stat.identity == FileIdentity{}
                             ? !seen_paths.insert(entry.path.native()).second
                             : std::ranges::find(seen, entry.stat.identity) != seen.end();
      if (known) {
        continue;
      }
      seen.push_back(entry.stat.identity);
      disks.push_back(VirtualDisk{.path = entry.path,
                                  .area = source.area,
                                  .label = source.label,
                                  .distribution = source.distribution,
                                  .size = entry.stat.size,
                                  .size_on_disk = entry.stat.size_on_disk,
                                  .identity = entry.stat.identity,
                                  .in_use = usage != nullptr && usage->locked(entry.path)});
    }
  }
  std::ranges::sort(disks, [](const VirtualDisk& left, const VirtualDisk& right) {
    return left.size != right.size ? left.size > right.size : left.path < right.path;
  });
  return disks;
}

void to_json(nlohmann::json& json, const VirtualDisk& disk) {
  json = {{"path", utf8_text(disk.path)},
          {"area", disk.area},
          {"label", disk.label},
          {"distribution",
           disk.distribution.empty() ? nlohmann::json(nullptr) : nlohmann::json(disk.distribution)},
          {"size", disk.size.count()},
          {"size_on_disk", disk.size_on_disk.count()},
          {"in_use", disk.in_use}};
}

}  // namespace myclean::vhdx
