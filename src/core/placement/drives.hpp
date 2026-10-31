#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/health/collector.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::placement {

// What kind of storage a drive is, in order of preference for build-heavy data (TEMP, package
// caches, the page file): many small random writes favour a TLC NVMe SSD.
enum class DriveClass {
  nvme,      // NVMe SSD
  sata_ssd,  // SSD on another bus (no seek penalty)
  nvme_qlc,  // NVMe SSD with QLC flash: slow once its SLC cache fills, lower endurance
  hdd,       // rotating disk (seek penalty)
  usb,       // attached over USB: may be unplugged
  unknown,
};

[[nodiscard]] std::string_view to_string(DriveClass drive_class);

// True for the QLC models MyClean knows, by a case-insensitive part of the model name Windows
// reports: Crucial P1 ("P1SSD8"), P3 ("P3SSD8") and P3 Plus ("P3PSSD8"), Samsung QVO ("QVO"),
// Intel 660p/670p ("SSDPEKNW", "SSDPEKNU") and Sabrent Rocket Q ("ROCKET Q").
[[nodiscard]] bool known_qlc_model(std::string_view model);

// The heuristic: USB bus -> usb; a seek penalty -> hdd; NVMe -> nvme_qlc for a known QLC model,
// else nvme; no seek penalty on another bus -> sata_ssd; otherwise unknown.
[[nodiscard]] DriveClass classify(health::Bus bus, std::string_view model,
                                  std::optional<bool> seek_penalty);

// Whether a physical disk has a seek penalty (rotates). Implemented by src/platform/settings.
class ISeekPenalty {
 public:
  virtual ~ISeekPenalty() = default;
  ISeekPenalty(const ISeekPenalty&) = delete;
  ISeekPenalty& operator=(const ISeekPenalty&) = delete;
  ISeekPenalty(ISeekPenalty&&) = delete;
  ISeekPenalty& operator=(ISeekPenalty&&) = delete;

  // nullopt when the disk does not say.
  [[nodiscard]] virtual std::optional<bool> seek_penalty(std::uint32_t disk_number) const = 0;

 protected:
  ISeekPenalty() = default;
};

// A volume with a drive letter as a possible target.
struct TargetVolume {
  std::filesystem::path root;  // "D:\"
  Bytes size;
  Bytes free;
  DriveClass drive_class = DriveClass::unknown;  // the least preferred of its disks
  bool healthy = false;                          // every disk under it is assessed `good`
  std::string health_note;                       // why it is not healthy; empty when it is
};

// Joins the volumes with the latest health of their disks (null before the first read: every
// volume is then of unknown health) and the seek penalty of each disk. Volumes without a drive
// letter are left out.
[[nodiscard]] std::vector<TargetVolume> target_volumes(
    std::span<const telemetry::VolumeStatus> volumes, const health::HealthView* health,
    const ISeekPenalty& seek_penalty);

// The current volumes as targets. Read again at apply time, so a drive that degraded after the
// proposal is refused.
class IDriveFacts {
 public:
  virtual ~IDriveFacts() = default;
  IDriveFacts(const IDriveFacts&) = delete;
  IDriveFacts& operator=(const IDriveFacts&) = delete;
  IDriveFacts(IDriveFacts&&) = delete;
  IDriveFacts& operator=(IDriveFacts&&) = delete;

  [[nodiscard]] virtual Result<std::vector<TargetVolume>> volumes() const = 0;

 protected:
  IDriveFacts() = default;
};

// IDriveFacts over the volume source, the health collector's latest view and the seek penalty.
// Every reference must outlive it.
class LiveDriveFacts final : public IDriveFacts {
 public:
  using HealthLatest = std::function<std::shared_ptr<const health::HealthView>()>;

  LiveDriveFacts(const telemetry::IVolumeInfo& volumes, const ISeekPenalty& seek_penalty,
                 HealthLatest health)
      : volumes_(&volumes), seek_penalty_(&seek_penalty), health_(std::move(health)) {}

  [[nodiscard]] Result<std::vector<TargetVolume>> volumes() const override;

 private:
  const telemetry::IVolumeInfo* volumes_;
  const ISeekPenalty* seek_penalty_;
  HealthLatest health_;
};

// "D:\" for a path on drive D (upper case); empty for a path without a drive letter.
[[nodiscard]] std::wstring drive_root(const std::filesystem::path& path);

// True when both name the same path, ignoring case and trailing separators.
[[nodiscard]] bool same_path(const std::filesystem::path& left, const std::filesystem::path& right);

// True when `child` lies below `parent`, compared as same_path() does.
[[nodiscard]] bool inside(const std::filesystem::path& child, const std::filesystem::path& parent);

// The volume that holds `path` (by drive root, case-insensitive); null when none does.
[[nodiscard]] const TargetVolume* volume_of(std::span<const TargetVolume> volumes,
                                            const std::filesystem::path& path);

// Why `volume` cannot take `needed` more bytes; empty when it can. It must be healthy (a drive
// whose health is degraded or unknown is refused) and keep at least 10 % of its size free
// afterwards.
[[nodiscard]] std::string unsuitable(const TargetVolume& volume, Bytes needed);

// The best volume for `needed` bytes now on the volume of `source`: another volume that is not
// unsuitable(), preferring the drive class (DriveClass order), then the most free space. So a
// QLC, HDD or USB volume is chosen only when no healthy NVMe or SATA SSD volume has room. Null
// when none qualifies.
[[nodiscard]] const TargetVolume* best_target(std::span<const TargetVolume> volumes,
                                              const std::filesystem::path& source, Bytes needed);

}  // namespace myclean::placement
