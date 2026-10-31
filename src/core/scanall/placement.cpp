#include "core/scanall/placement.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/map/attribution.hpp"
#include "core/map/disk_map.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/placement/relocation.hpp"
#include "core/scanall/cold.hpp"
#include "core/scanall/findings.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

namespace fs = std::filesystem;
using placement::DriveClass;

constexpr std::array<DriveClass, 2> fast_classes = {DriveClass::nvme, DriveClass::sata_ssd};

struct Context {
  const DriveInfo* drive = nullptr;
  std::span<const DriveInfo> drives;
  std::vector<Finding>* findings = nullptr;
};

// A relocate finding with a move_folder plan to the best drive of `classes`.
void add_move(const Context& context, const fs::path& path, Bytes size,
              std::span<const DriveClass> classes, std::string reason, double confidence) {
  Finding finding{.group = ActionGroup::relocate,
                  .source = "placement",
                  .drive = context.drive->root,
                  .path = path,
                  .items = {Item{.path = path, .size = size}},
                  .bytes = size,
                  .confidence = confidence,
                  .reason = std::move(reason)};
  const DriveInfo* target = best_target(context.drives, path, size, classes);
  if (target == nullptr) {
    finding.group = ActionGroup::manual_review;
    finding.reason += "; no suitable healthy drive keeps 10 % free after taking it";
  } else {
    finding.plan = Plan{.kind = PlanKind::move_folder,
                        .params = {{"source", space::utf8(path)},
                                   {"target", space::utf8(target->root / path.relative_path())}}};
  }
  context.findings->push_back(std::move(finding));
}

bool failing(const DriveInfo& drive) {
  return drive.health == health::Status::warning || drive.health == health::Status::critical;
}

void move_off(const Context& context, const space::ScanResult& tree,
              const PlacementOptions& options) {
  const DriveInfo& drive = *context.drive;
  for (const std::size_t index : tree.root().folders) {
    const space::Folder& folder = tree.folders[index];
    if (folder.size_on_disk < options.min_size || system_location(folder.path)) {
      continue;
    }
    add_move(context, folder.path, folder.size_on_disk, internal_classes,
             std::format("the drive's health is {}: {}; copy the data off it first",
                         health::to_string(drive.health), drive.health_note),
             0.9);
    context.findings->back().urgent = true;
  }
}

bool is_project(const space::ScanResult& tree, const space::Folder& folder) {
  const auto git = [](const fs::path& name) { return name == L".git"; };
  return std::ranges::any_of(
             folder.folders,
             [&](std::size_t child) { return git(tree.folders[child].path.filename()); }) ||
         std::ranges::any_of(folder.files,
                             [&](const space::FileItem& file) { return git(file.name); });
}

void active_projects(const Context& context, const space::ScanResult& tree,
                     const FolderTimes& times, WallTime now, const PlacementOptions& options) {
  const DriveClass drive_class = context.drive->drive_class;
  if (drive_class != DriveClass::nvme_qlc && drive_class != DriveClass::usb) {
    return;
  }
  std::vector<bool> closed(tree.folders.size(), false);
  for (std::size_t index = 1; index < tree.folders.size(); ++index) {
    const space::Folder& folder = tree.folders[index];
    closed[index] = closed[folder.parent] || folder.size_on_disk < options.min_project;
    if (closed[index] || !is_project(tree, folder)) {
      continue;
    }
    closed[index] = true;
    if (times.newest[index] >= now - options.active) {
      add_move(context, folder.path, folder.size_on_disk, fast_classes,
               std::format("an active project (changed {:%Y-%m-%d}) on a {} drive: builds and "
                           "git write many small files, which a TLC NVMe SSD handles faster",
                           std::chrono::floor<std::chrono::days>(times.newest[index]),
                           placement::to_string(drive_class)),
               0.6);
    }
  }
}

void games_on_usb(const Context& context, const map::DiskMap& map,
                  const PlacementOptions& options) {
  if (context.drive->drive_class != DriveClass::usb) {
    return;
  }
  for (const map::Match& match : map.matches) {
    const map::Area& area = map.areas[match.area];
    if (area.category == map::Category::games && !match.file && match.size >= options.min_project) {
      add_move(context, match.path, match.size, fast_classes,
               area.label +
                   " on a USB drive: an internal SSD loads it faster and never "
                   "unplugs; move it with its launcher",
               0.6);
    }
  }
}

void caches_on_system(const Context& context, const map::DiskMap& map,
                      const PlacementOptions& options) {
  if (!context.drive->system) {
    return;
  }
  for (const map::Match& match : map.matches) {
    const map::Area& area = map.areas[match.area];
    if (area.reclaim.kind != map::ReclaimKind::relocate || match.size < options.min_project) {
      continue;
    }
    Finding finding{.group = ActionGroup::relocate,
                    .source = "placement",
                    .drive = context.drive->root,
                    .path = match.path,
                    .items = {Item{.path = match.path, .size = match.size}},
                    .bytes = match.size,
                    .confidence = 0.8,
                    .reason = area.label + " on the system drive" +
                              (area.reclaim.note.empty() ? "" : ": " + area.reclaim.note)};
    const DriveInfo* target = best_target(context.drives, match.path, match.size, internal_classes);
    if (target == nullptr) {
      finding.group = ActionGroup::manual_review;
      finding.reason += "; no healthy drive keeps 10 % free after taking it";
    } else {
      const std::optional<placement::CacheKind> kind =
          placement::parse_cache_kind(area.reclaim.ref);
      const fs::path folder = kind ? placement::target_folder(*kind, target->root)
                                   : target->root / match.path.filename();
      finding.plan = Plan{.kind = PlanKind::relocate_cache,
                          .params = {{"cache", area.reclaim.ref},
                                     {"source", space::utf8(match.path)},
                                     {"target", space::utf8(folder)}}};
    }
    context.findings->push_back(std::move(finding));
  }
}

}  // namespace

std::vector<Finding> advise_placement(const space::ScanResult& tree, const FolderTimes& times,
                                      const map::DiskMap* map, const DriveInfo& drive,
                                      std::span<const DriveInfo> drives, WallTime now,
                                      const PlacementOptions& options) {
  std::vector<Finding> findings;
  const Context context{.drive = &drive, .drives = drives, .findings = &findings};
  if (failing(drive)) {
    move_off(context, tree, options);
    return findings;
  }
  active_projects(context, tree, times, now, options);
  if (map != nullptr) {
    games_on_usb(context, *map, options);
    caches_on_system(context, *map, options);
  }
  return findings;
}

}  // namespace myclean::scanall
