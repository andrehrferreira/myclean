#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "core/drives/analysis.hpp"
#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/os/clock.hpp"
#include "core/placement/settings.hpp"
#include "core/store/store.hpp"

// The maintenance actions RetrimVolume and DefragVolume, run by the service with Windows' own
// defrag tool, and the conditions they run under: the drive healthy, its disks idle, the computer
// not on battery.
namespace myclean::drives {

// How idle a disk has been. StoreDiskIdle reads the telemetry series; tests use fakes.
class IDiskIdle {
 public:
  virtual ~IDiskIdle() = default;
  IDiskIdle(const IDiskIdle&) = delete;
  IDiskIdle& operator=(const IDiskIdle&) = delete;
  IDiskIdle(IDiskIdle&&) = delete;
  IDiskIdle& operator=(IDiskIdle&&) = delete;

  // The mean idle time of the disk over the last `window`, 0-100; nullopt when there are too
  // few samples to tell.
  [[nodiscard]] virtual Result<std::optional<double>> idle_percent(
      std::uint32_t disk_number, std::chrono::seconds window) const = 0;

 protected:
  IDiskIdle() = default;
};

// IDiskIdle over the disk.idle series (1 s samples, entity the disk number): the mean of the
// window's samples, nullopt when they cover less than half of it. Both references must outlive
// it.
class StoreDiskIdle final : public IDiskIdle {
 public:
  StoreDiskIdle(store::Store& store, const os::IClock& clock) : store_(&store), clock_(&clock) {}

  [[nodiscard]] Result<std::optional<double>> idle_percent(
      std::uint32_t disk_number, std::chrono::seconds window) const override;

 private:
  store::Store* store_;
  const os::IClock* clock_;
};

// Whether the computer runs on battery. Implemented by src/platform/defrag.
class IPowerSource {
 public:
  virtual ~IPowerSource() = default;
  IPowerSource(const IPowerSource&) = delete;
  IPowerSource& operator=(const IPowerSource&) = delete;
  IPowerSource(IPowerSource&&) = delete;
  IPowerSource& operator=(IPowerSource&&) = delete;

  [[nodiscard]] virtual Result<bool> on_battery() const = 0;

 protected:
  IPowerSource() = default;
};

struct RunPolicy {
  std::chrono::minutes idle_window{10};
  double min_idle_percent = 90.0;  // every disk of the volume, over idle_window
};

// Why `kind` (retrim or defrag) does not apply to `drive`; empty when it does. Only an SSD that
// does not refuse TRIM is retrimmed; only an HDD is defragmented.
[[nodiscard]] std::string unsuitable(Maintenance kind, const DriveState& drive);

// Why no maintenance may run on `drive` now; empty when it may: its health is not good, the
// computer runs on battery, or a disk of it was less than min_idle_percent idle over the window
// (or has too little history to tell).
[[nodiscard]] Result<std::string> blocked(const DriveState& drive, const IDiskIdle& idle,
                                          const IPowerSource& power, const RunPolicy& policy);

// "defrag K: /D" or "defrag D: /L" for the volume at `root`: Windows' storage optimizer at its
// default low priority (no /H). `kind` is retrim or defrag.
[[nodiscard]] placement::Command maintenance_command(Maintenance kind,
                                                     const std::filesystem::path& root);

struct MaintenanceTools {
  AnalysisTools analysis;
  const IDriveStates* drives = nullptr;
  const IDiskIdle* idle = nullptr;
  const IPowerSource* power = nullptr;
  // Runs defrag.exe; its timeout must allow a long defragmentation (hours on a large HDD).
  placement::IProcessRunner* runner = nullptr;
};

// One maintenance run on one volume. Preview is the volume's analysis, verify analyses again,
// and nothing is undone: a retrim or defragmentation needs no undo and has none.
class VolumeMaintenance : public Action {
 public:
  [[nodiscard]] ActionInfo describe() const override;
  // One item, the volume root (nothing is freed). Fails with refused when unsuitable().
  [[nodiscard]] Result<Preview> preview() override;
  // Reads the drive again and refuses (Errc::refused, with the reason) when unsuitable() or
  // blocked(); otherwise runs maintenance_command() and fails when it exits non-zero.
  [[nodiscard]] Result<Undo> apply(const Preview& preview) override;
  // Metric "days_since_retrim" (retrim) or "fragmented_percent" (defrag), before and after;
  // lower is better, -1 = unknown.
  [[nodiscard]] Result<Verification> verify(const Undo& undo) override;
  // Always fails with not_reversible.
  [[nodiscard]] Result<void> undo(const Undo& undo) override;

  // The analysis of the last preview(); null before one.
  [[nodiscard]] const std::optional<VolumeAnalysis>& analysis() const { return analysis_; }

 protected:
  VolumeMaintenance(Maintenance kind, std::filesystem::path root, const MaintenanceTools& tools,
                    const AnalysisOptions& options, RunPolicy policy)
      : kind_(kind), root_(std::move(root)), tools_(tools), options_(options), policy_(policy) {}

 private:
  [[nodiscard]] Result<VolumeAnalysis> current() const;
  [[nodiscard]] double metric(const VolumeAnalysis& analysis) const;

  Maintenance kind_;
  std::filesystem::path root_;
  MaintenanceTools tools_;
  AnalysisOptions options_;
  RunPolicy policy_;
  std::optional<VolumeAnalysis> analysis_;
};

// Retrims an SSD volume (tier auto: harmless, short).
class RetrimVolume final : public VolumeMaintenance {
 public:
  RetrimVolume(std::filesystem::path root, const MaintenanceTools& tools,
               const AnalysisOptions& options = {}, RunPolicy policy = {})
      : VolumeMaintenance(Maintenance::retrim, std::move(root), tools, options, policy) {}
};

// Defragments an HDD volume (tier ask: hours of disk load).
class DefragVolume final : public VolumeMaintenance {
 public:
  DefragVolume(std::filesystem::path root, const MaintenanceTools& tools,
               const AnalysisOptions& options = {}, RunPolicy policy = {})
      : VolumeMaintenance(Maintenance::defrag, std::move(root), tools, options, policy) {}
};

}  // namespace myclean::drives
