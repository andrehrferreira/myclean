#include "core/placement/drives.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/model/enum_names.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::placement {

namespace {

constexpr std::array<std::string_view, 6> drive_class_names = {"nvme", "sata_ssd", "nvme_qlc",
                                                               "hdd",  "usb",      "unknown"};

constexpr std::array<std::string_view, 7> qlc_model_parts = {
    "P1SSD8", "P3SSD8", "P3PSSD8", "QVO", "SSDPEKNW", "SSDPEKNU", "ROCKET Q"};

std::string upper(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
  }
  return result;
}

// Upper-cased, without trailing separators beyond a drive root: the form paths compare in.
std::wstring folded(const std::filesystem::path& path) {
  std::wstring text = path.native();
  for (wchar_t& character : text) {
    character = static_cast<wchar_t>(std::towupper(character));
  }
  while (text.size() > 3 && text.back() == L'\\') {
    text.pop_back();
  }
  return text;
}

const health::DriveHealth* health_of(const health::HealthView* view, std::uint32_t disk) {
  if (view == nullptr) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(view->drives, [&](const health::DriveHealth& drive) {
    return drive.reading.disk_number == disk;
  });
  return found == view->drives.end() ? nullptr : &*found;
}

TargetVolume target_of(const telemetry::VolumeStatus& volume, std::wstring root,
                       const health::HealthView* view, const ISeekPenalty& seek_penalty) {
  TargetVolume target{.root = std::move(root),
                      .size = volume.size,
                      .free = volume.free,
                      .drive_class = volume.disks.empty() ? DriveClass::unknown : DriveClass::nvme,
                      .healthy = !volume.disks.empty()};
  if (volume.disks.empty()) {
    target.health_note = "its physical disks are unknown";
  }
  for (const std::uint32_t disk : volume.disks) {
    const health::DriveHealth* drive = health_of(view, disk);
    if (drive == nullptr) {
      target.drive_class = DriveClass::unknown;
      target.healthy = false;
      target.health_note = "no health reading for disk " + std::to_string(disk);
      continue;
    }
    target.drive_class = std::max(
        target.drive_class,
        classify(drive->reading.bus, drive->reading.model, seek_penalty.seek_penalty(disk)));
    if (drive->assessment.status != health::Status::good) {
      target.healthy = false;
      target.health_note = "disk " + std::to_string(disk) + " (" + drive->reading.model +
                           ") health is " +
                           std::string(health::to_string(drive->assessment.status));
      for (const std::string& reason : drive->assessment.reasons) {
        target.health_note += "; " + reason;
      }
    }
  }
  return target;
}

}  // namespace

std::string_view to_string(DriveClass drive_class) {
  return detail::enum_name(drive_class_names, drive_class);
}

bool known_qlc_model(std::string_view model) {
  const std::string name = upper(model);
  return std::ranges::any_of(
      qlc_model_parts, [&](std::string_view part) { return name.find(part) != std::string::npos; });
}

DriveClass classify(health::Bus bus, std::string_view model, std::optional<bool> seek_penalty) {
  if (bus == health::Bus::usb) {
    return DriveClass::usb;
  }
  if (seek_penalty == true) {
    return DriveClass::hdd;
  }
  if (bus == health::Bus::nvme) {
    return known_qlc_model(model) ? DriveClass::nvme_qlc : DriveClass::nvme;
  }
  return seek_penalty == false ? DriveClass::sata_ssd : DriveClass::unknown;
}

std::vector<TargetVolume> target_volumes(std::span<const telemetry::VolumeStatus> volumes,
                                         const health::HealthView* health,
                                         const ISeekPenalty& seek_penalty) {
  std::vector<TargetVolume> targets;
  for (const telemetry::VolumeStatus& volume : volumes) {
    const auto letter = std::ranges::find_if(volume.mount_points, [](const std::string& point) {
      return point.size() == 3 && std::isalpha(static_cast<unsigned char>(point[0])) != 0 &&
             point.substr(1) == ":\\";
    });
    if (letter != volume.mount_points.end()) {
      const auto drive =
          static_cast<wchar_t>(std::toupper(static_cast<unsigned char>((*letter)[0])));
      targets.push_back(target_of(volume, {drive, L':', L'\\'}, health, seek_penalty));
    }
  }
  return targets;
}

Result<std::vector<TargetVolume>> LiveDriveFacts::volumes() const {
  Result<std::vector<telemetry::VolumeStatus>> volumes = volumes_->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  const std::shared_ptr<const health::HealthView> view = health_();
  return target_volumes(*volumes, view.get(), *seek_penalty_);
}

std::wstring drive_root(const std::filesystem::path& path) {
  const std::wstring& text = path.native();
  if (text.size() < 2 || text[1] != L':' || std::iswalpha(text[0]) == 0) {
    return {};
  }
  return {static_cast<wchar_t>(std::towupper(text[0])), L':', L'\\'};
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
  return folded(left) == folded(right);
}

bool inside(const std::filesystem::path& child, const std::filesystem::path& parent) {
  const std::wstring inner = folded(child);
  const std::wstring outer = folded(parent);
  return !outer.empty() && inner.size() > outer.size() && inner.starts_with(outer) &&
         (outer.back() == L'\\' || inner[outer.size()] == L'\\');
}

const TargetVolume* volume_of(std::span<const TargetVolume> volumes,
                              const std::filesystem::path& path) {
  const std::wstring root = drive_root(path);
  if (root.empty()) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      volumes, [&](const TargetVolume& volume) { return drive_root(volume.root) == root; });
  return found == volumes.end() ? nullptr : &*found;
}

std::string unsuitable(const TargetVolume& volume, Bytes needed) {
  if (!volume.healthy) {
    return "the drive's health is not good: " + volume.health_note;
  }
  const std::uint64_t reserve = volume.size.count() / 10;
  if (volume.free.count() < needed.count() + reserve) {
    return "it would keep less than 10 % of its size free";
  }
  return {};
}

const TargetVolume* best_target(std::span<const TargetVolume> volumes,
                                const std::filesystem::path& source, Bytes needed) {
  const std::wstring source_root = drive_root(source);
  const TargetVolume* best = nullptr;
  for (const TargetVolume& volume : volumes) {
    if (drive_root(volume.root) == source_root || !unsuitable(volume, needed).empty()) {
      continue;
    }
    if (best == nullptr || volume.drive_class < best->drive_class ||
        (volume.drive_class == best->drive_class && volume.free > best->free)) {
      best = &volume;
    }
  }
  return best;
}

}  // namespace myclean::placement
