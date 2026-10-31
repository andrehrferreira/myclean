#include "core/scanall/findings.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/units.hpp"
#include "core/placement/drives.hpp"
#include "core/space/text.hpp"

namespace myclean::scanall {

namespace {

constexpr std::array<std::string_view, action_group_count> group_names = {
    "remove",   "deduplicate", "relocate",         "archive",
    "compress", "defrag-trim", "uninstall-review", "manual-review"};

constexpr std::array<std::string_view, 14> plan_names = {
    "delete_items",   "quarantine_items", "native_command", "cleanup_handler", "compress_folder",
    "relocate_cache", "hibernation",      "page_file",      "shadow_storage",  "retrim_volume",
    "defrag_volume",  "move_folder",      "uninstall",      "review"};

// Lower case; the folders Windows keeps in the root of a volume.
constexpr std::array<std::string_view, 12> system_folders = {"windows",
                                                             "program files",
                                                             "program files (x86)",
                                                             "programdata",
                                                             "recovery",
                                                             "$recycle.bin",
                                                             "system volume information",
                                                             "$winreagent",
                                                             "config.msi",
                                                             "perflogs",
                                                             "$windows.~bt",
                                                             "$windows.~ws"};

}  // namespace

std::string_view to_string(ActionGroup group) {
  return detail::enum_name(group_names, group);
}

std::string_view to_string(PlanKind kind) {
  return detail::enum_name(plan_names, kind);
}

const std::string* Plan::param(std::string_view name) const {
  const auto found = std::ranges::find(params, name, &std::pair<std::string, std::string>::first);
  return found == params.end() ? nullptr : &found->second;
}

Bytes footprint(const Finding& finding) {
  Bytes total;
  for (const Item& item : finding.items) {
    total += item.size;
  }
  return total;
}

bool healthy(const DriveInfo& drive) {
  return drive.health == health::Status::good;
}

const DriveInfo* drive_of(std::span<const DriveInfo> drives, const std::filesystem::path& path) {
  const std::wstring root = placement::drive_root(path);
  if (root.empty()) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(drives, [&root](const DriveInfo& drive) {
    return placement::drive_root(drive.root) == root;
  });
  return found == drives.end() ? nullptr : &*found;
}

const DriveInfo* best_target(std::span<const DriveInfo> drives, const std::filesystem::path& source,
                             Bytes needed, std::span<const placement::DriveClass> classes) {
  std::vector<placement::TargetVolume> volumes;
  for (const DriveInfo& drive : drives) {
    if (std::ranges::find(classes, drive.drive_class) != classes.end()) {
      volumes.push_back(placement::TargetVolume{.root = drive.root,
                                                .size = drive.size,
                                                .free = drive.free,
                                                .drive_class = drive.drive_class,
                                                .healthy = healthy(drive),
                                                .health_note = drive.health_note});
    }
  }
  const placement::TargetVolume* target = placement::best_target(volumes, source, needed);
  return target == nullptr ? nullptr : drive_of(drives, target->root);
}

bool system_location(const std::filesystem::path& path) {
  const std::filesystem::path relative = path.relative_path();
  if (relative.empty()) {
    return false;
  }
  const std::string first = space::lower_ascii(space::utf8(*relative.begin()));
  return std::ranges::find(system_folders, first) != system_folders.end();
}

std::string size_text(Bytes bytes) {
  constexpr std::array<std::string_view, 5> units = {"B", "KiB", "MiB", "GiB", "TiB"};
  auto value = static_cast<double>(bytes.count());
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < units.size()) {
    value /= 1024.0;
    ++unit;
  }
  return unit == 0 ? std::format("{} B", bytes.count())
                   : std::format("{:.1f} {}", value, units.at(unit));
}

}  // namespace myclean::scanall
