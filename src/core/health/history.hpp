#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/health/analysis.hpp"
#include "core/health/reading.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"

namespace myclean::health {

// The health tables (component "health"), registered with store::Store::open:
//   health_sample  one row per drive and local day: the last reading of that day, its source,
//                  status and every counter the drive reported (NULL when not reported).
//                  Kept 2 years (Store::history_retention).
[[nodiscard]] const store::MigrationSet& health_migrations();

// One stored daily snapshot.
struct Snapshot {
  std::string day;  // local date, "2026-10-31"
  WallTime time;    // the reading's time
  Status status = Status::unknown;
  HealthReading reading;
};

// Health history in the store. Every call is one synchronous Store::write or Store::read.
class HealthHistory {
 public:
  // `store` must outlive the history.
  explicit HealthHistory(store::Store& store) : store_(&store) {}

  // Adds the 2-year retention rule for health_sample to the store.
  [[nodiscard]] Result<void> add_retention();

  // Stores `reading` as the snapshot of its local day in `zone`, replacing an earlier reading of
  // the same drive and day: the snapshot of a day is its last reading.
  [[nodiscard]] Result<void> save(const HealthReading& reading, Status status, WallTime time,
                                  const std::chrono::time_zone& zone);
  // The snapshots of `drive` taken in [from, to), oldest first.
  [[nodiscard]] Result<std::vector<Snapshot>> history(std::string_view drive, WallTime from,
                                                      WallTime to);
  // The oldest snapshot of `drive` taken at or after `since` that reports the data written.
  [[nodiscard]] Result<std::optional<WrittenAt>> first_written_since(std::string_view drive,
                                                                     WallTime since);

 private:
  store::Store* store_;
};

}  // namespace myclean::health
