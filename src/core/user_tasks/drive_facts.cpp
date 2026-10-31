#include "core/user_tasks/drive_facts.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "core/placement/drives.hpp"
#include "core/tray_model/snapshot.hpp"

namespace myclean::user_tasks {

std::vector<placement::TargetVolume> target_volumes(
    const std::vector<tray_model::VolumeRow>& volumes,
    const std::optional<std::vector<tray_model::DriveHealthRow>>& drives) {
  std::vector<placement::TargetVolume> targets;
  for (const tray_model::VolumeRow& volume : volumes) {
    const std::string& name = volume.name;
    if (name.size() != 2 || name[1] != ':') {
      continue;  // mounted in a folder: no drive letter
    }
    placement::TargetVolume target{.root = std::wstring{static_cast<wchar_t>(name[0]), L':', L'\\'},
                                   .size = volume.size,
                                   .free = volume.free,
                                   .drive_class = placement::DriveClass::unknown,
                                   .healthy = !volume.disks.empty(),
                                   .health_note = {}};
    for (const std::string& disk : volume.disks) {
      const tray_model::DriveHealthRow* drive = nullptr;
      if (drives) {
        const auto found = std::ranges::find(*drives, disk, &tray_model::DriveHealthRow::disk);
        drive = found == drives->end() ? nullptr : &*found;
      }
      if (drive == nullptr || drive->status != tray_model::DriveStatus::good) {
        target.healthy = false;
        target.health_note =
            "the health of disk " + disk + " is " +
            std::string(drive == nullptr ? "unknown" : tray_model::to_string(drive->status));
        break;
      }
    }
    if (volume.disks.empty()) {
      target.health_note = "no physical disk is known for it";
    }
    targets.push_back(std::move(target));
  }
  return targets;
}

}  // namespace myclean::user_tasks
