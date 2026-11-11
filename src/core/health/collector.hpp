#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/health/drive_specs.hpp"
#include "core/health/history.hpp"
#include "core/health/reading.hpp"
#include "core/health/source.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/clock.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/collector.hpp"

namespace myclean::health {

// Series recorded at every read, in °C. The entity is the drive's stable identity
// (HealthReading::drive, "<model>#<serial hash>", as the alerts use it), not its disk number,
// which can change when drives are added or removed.
inline constexpr std::string_view metric_disk_temperature = "disk.temperature";

// The current health of one drive.
struct DriveHealth {
  HealthReading reading;
  Assessment assessment;
  Endurance endurance;
};

struct HealthView {
  WallTime time;
  std::vector<DriveHealth> drives;  // in disk number order
};

// Reads the health of every drive every 10 min (as a pipeline collector, so the service's
// telemetry thread drives it): interprets and assesses each reading, reports its alert facts
// (alert_readings) to the alert sink, stores it as the drive's snapshot of the day, projects
// its write endurance from the last year of snapshots, and records its temperature.
class HealthCollector final : public telemetry::ICollector {
 public:
  static constexpr std::chrono::minutes read_interval{10};
  // The window of history used for the write rate.
  static constexpr std::chrono::days rate_window{365};

  // Adds the health_sample retention rule. Every reference must outlive the collector; the
  // store must have been opened with health_migrations().
  [[nodiscard]] static Result<std::unique_ptr<HealthCollector>> create(
      const IDiskHealthSource& source, store::Store& store, DriveSpecs specs, IAlertSink& alerts,
      const os::IClock& clock);

  [[nodiscard]] std::string_view name() const override { return "disk_health"; }
  [[nodiscard]] std::chrono::seconds interval() const override { return read_interval; }
  // Fails when the disks cannot be enumerated or a snapshot cannot be stored; the readings,
  // alerts and view are still produced in the second case.
  [[nodiscard]] Result<std::vector<telemetry::Measurement>> collect(WallTime now,
                                                                    MonoTime monotonic) override;

  // The newest reading of every drive; null before the first read. Thread-safe.
  [[nodiscard]] std::shared_ptr<const HealthView> latest() const { return latest_.get(); }
  // The daily snapshots of `drive` in [from, to), oldest first. Thread-safe.
  [[nodiscard]] Result<std::vector<Snapshot>> history(std::string_view drive, WallTime from,
                                                      WallTime to) const;

 private:
  HealthCollector(const IDiskHealthSource& source, store::Store& store, DriveSpecs specs,
                  IAlertSink& alerts, const os::IClock& clock);

  const IDiskHealthSource* source_;
  mutable HealthHistory history_;
  DriveSpecs specs_;
  IAlertSink* alerts_;
  const os::IClock* clock_;
  telemetry::Latest<HealthView> latest_;
};

}  // namespace myclean::health
