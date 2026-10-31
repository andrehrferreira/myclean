#pragma once

#include <chrono>
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

#include "core/drives/history.hpp"
#include "core/drives/layout.hpp"
#include "core/health/collector.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/placement/drives.hpp"
#include "core/space/scanner.hpp"
#include "core/telemetry/volume_info.hpp"

// Drive maintenance analysis (drive-maintenance spec): per volume its medium and class, TRIM
// support and last retrim for an SSD, a fragmentation sample and the free-space layout for an
// HDD, and what maintenance it needs, with the reasons.
namespace myclean::drives {

// The storage medium: decides which maintenance applies. An SSD is retrimmed, never
// defragmented; an HDD is defragmented, never retrimmed.
enum class Media { ssd, hdd, unknown };

[[nodiscard]] std::string_view to_string(Media media);

// TRIM as the platform reports it. Implemented by src/platform/defrag.
class ITrimSupport {
 public:
  virtual ~ITrimSupport() = default;
  ITrimSupport(const ITrimSupport&) = delete;
  ITrimSupport& operator=(const ITrimSupport&) = delete;
  ITrimSupport(ITrimSupport&&) = delete;
  ITrimSupport& operator=(ITrimSupport&&) = delete;

  // Whether the disk accepts TRIM (StorageDeviceTrimProperty); nullopt when it does not say.
  [[nodiscard]] virtual std::optional<bool> trim_supported(std::uint32_t disk_number) const = 0;
  // Whether NTFS sends TRIM on delete (DisableDeleteNotification is 0); nullopt when unreadable.
  [[nodiscard]] virtual std::optional<bool> ntfs_trim_enabled() const = 0;

 protected:
  ITrimSupport() = default;
};

// One volume with a drive letter, as maintenance sees it.
struct DriveState {
  std::filesystem::path root;  // "K:\"
  std::vector<std::uint32_t> disks;
  placement::DriveClass drive_class = placement::DriveClass::unknown;
  Media media = Media::unknown;  // the same on every disk, else unknown
  bool healthy = false;          // every disk assessed `good`
  std::string health_note;       // why it is not healthy
  Bytes size;
  Bytes free;
  std::optional<bool> trim_supported;  // by every disk; false when one refuses it
  std::optional<bool> ntfs_trim_enabled;
};

// Joins the volumes with their disks' health (null before the first read), seek penalty and
// TRIM support. Volumes without a drive letter are left out.
[[nodiscard]] std::vector<DriveState> drive_states(std::span<const telemetry::VolumeStatus> volumes,
                                                   const health::HealthView* health,
                                                   const placement::ISeekPenalty& seek_penalty,
                                                   const ITrimSupport& trim);

// The current drive states; read again at apply time, so a drive that degraded is refused.
class IDriveStates {
 public:
  virtual ~IDriveStates() = default;
  IDriveStates(const IDriveStates&) = delete;
  IDriveStates& operator=(const IDriveStates&) = delete;
  IDriveStates(IDriveStates&&) = delete;
  IDriveStates& operator=(IDriveStates&&) = delete;

  [[nodiscard]] virtual Result<std::vector<DriveState>> drives() const = 0;

 protected:
  IDriveStates() = default;
};

// IDriveStates over the volume source, the health collector's latest view, the seek penalty
// and TRIM support. Every reference must outlive it.
class LiveDriveStates final : public IDriveStates {
 public:
  using HealthLatest = std::function<std::shared_ptr<const health::HealthView>()>;

  LiveDriveStates(const telemetry::IVolumeInfo& volumes, const placement::ISeekPenalty& seek,
                  const ITrimSupport& trim, HealthLatest health)
      : volumes_(&volumes), seek_(&seek), trim_(&trim), health_(std::move(health)) {}

  [[nodiscard]] Result<std::vector<DriveState>> drives() const override;

 private:
  const telemetry::IVolumeInfo* volumes_;
  const placement::ISeekPenalty* seek_;
  const ITrimSupport* trim_;
  HealthLatest health_;
};

enum class Maintenance { none, retrim, defrag };

[[nodiscard]] std::string_view to_string(Maintenance maintenance);

struct Thresholds {
  std::chrono::days retrim_age{30};     // retrim when the last one is older
  double defrag_percent = 10.0;         // defragment above this share of fragmented bytes
  std::uint64_t min_sample_files = 20;  // fewer sampled files decide nothing
};

struct AnalysisOptions {
  SampleBudget sample;
  std::chrono::milliseconds bitmap_budget{std::chrono::seconds{20}};
  Thresholds thresholds;
};

struct VolumeAnalysis {
  DriveState drive;
  LastRuns last;
  std::string history_note;                          // why the optimizer log could not be read
  std::optional<FragmentationSample> fragmentation;  // HDD only
  std::optional<FreeSpaceLayout> free_space;         // HDD only, when the bitmap was readable
  std::string free_space_note;                       // why it was not
  Maintenance needed = Maintenance::none;            // what the volume needs, health aside
  Maintenance recommended = Maintenance::none;       // `needed` while the drive is healthy
  std::vector<std::string> reasons;
};

// Sets needed, recommended and reasons of `analysis` from its facts:
//   SSD: retrim when TRIM is not refused and the last retrim is older than retrim_age or none is
//        in the log; never defragment
//   HDD: defragment when at least min_sample_files were sampled and their fragmented share is
//        above defrag_percent; never retrim
//   unknown medium: nothing
// A drive that is not healthy is recommended nothing, with the need and the health in reasons.
void recommend(VolumeAnalysis& analysis, WallTime now, const Thresholds& thresholds);

struct AnalysisTools {
  const space::IDirectoryReader* reader = nullptr;
  const IVolumeLayout* layout = nullptr;
  const IOptimizerLog* log = nullptr;
  const os::IClock* clock = nullptr;
};

// Analyses every drive: reads the optimizer log once, samples each HDD from its root and reads
// its bitmap within the budgets, then recommend(). Read-only; a read that fails is noted on the
// volume, never fatal.
[[nodiscard]] std::vector<VolumeAnalysis> analyze(std::span<const DriveState> drives,
                                                  const AnalysisTools& tools,
                                                  const AnalysisOptions& options);

}  // namespace myclean::drives
