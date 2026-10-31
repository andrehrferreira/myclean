#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::telemetry {

// The series entity of a volume: its first mount point without the trailing backslash ("C:").
[[nodiscard]] std::string volume_entity(const VolumeStatus& volume);

struct VolumeView {
  WallTime time;
  std::vector<VolumeStatus> volumes;
};

// Samples every mounted volume every 60 s: records volume.size and volume.free (see
// metrics.hpp); the volumes with their physical disks are available through latest().
class VolumeCollector final : public ICollector {
 public:
  // `volumes` must outlive the collector.
  explicit VolumeCollector(const IVolumeInfo& volumes);

  [[nodiscard]] std::string_view name() const override { return "volumes"; }
  [[nodiscard]] std::chrono::seconds interval() const override { return std::chrono::seconds{60}; }
  [[nodiscard]] Result<std::vector<Measurement>> collect(WallTime now, MonoTime monotonic) override;

  // The newest reading; null before the first sample. Thread-safe.
  [[nodiscard]] std::shared_ptr<const VolumeView> latest() const { return latest_.get(); }

 private:
  const IVolumeInfo* volumes_;
  Latest<VolumeView> latest_;
};

}  // namespace myclean::telemetry
