#include "core/scanall/cold.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/clean/patterns.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/scanall/findings.hpp"
#include "core/scanall/owner_decisions.hpp"
#include "core/space/scanner.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

namespace fs = std::filesystem;

constexpr std::array<placement::DriveClass, 3> ssd_classes = {
    placement::DriveClass::nvme, placement::DriveClass::sata_ssd, placement::DriveClass::nvme_qlc};

constexpr std::array<std::wstring_view, 3> archive_patterns = {L"*.zip", L"*.rar", L"*.7z"};

// A backup or a store of archives: named like a backup, or at least half of it is archives
// lying directly inside. Such a folder belongs on an HDD as a whole.
bool backup_like(const space::Folder& folder) {
  if (clean::wildcard_match(folder.path.filename().native(), L"*backup*")) {
    return true;
  }
  Bytes archives;
  for (const space::FileItem& file : folder.files) {
    if (std::ranges::any_of(archive_patterns, [&file](std::wstring_view pattern) {
          return clean::wildcard_match(file.name.native(), pattern);
        })) {
      archives += file.size_on_disk;
    }
  }
  return archives.count() > 0 && archives.count() * 2 >= folder.size_on_disk.count();
}

// By folder index: the bytes of the backup-like folders at or below it (the outermost only).
std::vector<Bytes> backup_bytes(const space::ScanResult& tree) {
  std::vector<Bytes> bytes(tree.folders.size());
  for (std::size_t index = tree.folders.size(); index-- > 0;) {
    const space::Folder& folder = tree.folders[index];
    if (backup_like(folder)) {
      bytes[index] = folder.size_on_disk;
    }
    if (folder.parent != space::no_parent) {
      bytes[folder.parent] += bytes[index];
    }
  }
  return bytes;
}

bool used_inside(std::span<const fs::path> in_use, const fs::path& folder) {
  return std::ranges::any_of(
      in_use, [&folder](const fs::path& used) { return clean::within(used, folder); });
}

Finding cold_finding(const space::Folder& folder, WallTime newest, const DriveInfo& drive,
                     std::span<const DriveInfo> drives) {
  const Bytes size = folder.size_on_disk;
  Finding finding{.group = ActionGroup::archive,
                  .source = "cold",
                  .drive = drive.root,
                  .path = folder.path,
                  .items = {Item{.path = folder.path, .size = size}},
                  .bytes = size,
                  .confidence = 0.7,
                  .reason = std::format("nothing inside changed since {:%Y-%m-%d}; {} on {}",
                                        std::chrono::floor<std::chrono::days>(newest),
                                        size_text(size), placement::to_string(drive.drive_class))};
  const std::optional<fs::path> destination = archive_target(drives, folder.path, size);
  if (!destination) {
    finding.group = ActionGroup::manual_review;
    finding.reason += "; no healthy HDD keeps 10 % free after taking it";
    return finding;
  }
  finding.plan =
      Plan{.kind = PlanKind::move_folder,
           .params = {{"source", space::utf8(folder.path)}, {"target", space::utf8(*destination)}}};
  return finding;
}

}  // namespace

FolderTimes folder_times(const space::ScanResult& tree) {
  const std::size_t count = tree.folders.size();
  FolderTimes times{.newest = std::vector<WallTime>(count),
                    .complete = std::vector<bool>(count, !tree.cancelled)};
  for (std::size_t index = 0; index < count; ++index) {
    const space::Folder& folder = tree.folders[index];
    for (const space::FileItem& file : folder.files) {
      times.newest[index] = std::max({times.newest[index], file.created, file.modified});
    }
    if (folder.error) {
      times.complete[index] = false;
    }
  }
  for (std::size_t index = count; index-- > 1;) {
    const std::size_t parent = tree.folders[index].parent;
    times.newest[parent] = std::max(times.newest[parent], times.newest[index]);
    if (!times.complete[index]) {
      times.complete[parent] = false;
    }
  }
  return times;
}

std::vector<Finding> find_cold(const space::ScanResult& tree, const FolderTimes& times,
                               const DriveInfo& drive, std::span<const DriveInfo> drives,
                               std::span<const fs::path> in_use, WallTime now,
                               const ColdOptions& options) {
  std::vector<Finding> findings;
  if (std::ranges::find(ssd_classes, drive.drive_class) == ssd_classes.end()) {
    return findings;
  }
  const WallTime cutoff = now - std::chrono::duration_cast<WallTime::duration>(
                                    std::chrono::months{options.min_age_months});
  const std::vector<Bytes> backups = backup_bytes(tree);
  // Closed: the folder and everything below it are decided (proposed, excluded or too small).
  std::vector<bool> closed(tree.folders.size(), false);
  for (std::size_t index = 1; index < tree.folders.size(); ++index) {
    const space::Folder& folder = tree.folders[index];
    if (closed[folder.parent] || folder.size_on_disk < options.min_size ||
        system_location(folder.path)) {
      closed[index] = true;
      continue;
    }
    // Backups next to min_size or more of other content (project trees): its parts are judged.
    const bool mixed =
        backups[index].count() > 0 && folder.size_on_disk >= backups[index] + options.min_size;
    if (!mixed && times.complete[index] && times.newest[index] < cutoff &&
        !used_inside(in_use, folder.path)) {
      findings.push_back(cold_finding(folder, times.newest[index], drive, drives));
      closed[index] = true;
    }
  }
  return findings;
}

}  // namespace myclean::scanall
