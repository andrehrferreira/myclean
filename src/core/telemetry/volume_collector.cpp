#include "core/telemetry/volume_collector.hpp"

#include <string>
#include <utility>
#include <vector>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/telemetry/collector.hpp"
#include "core/telemetry/metrics.hpp"
#include "core/telemetry/volume_info.hpp"

namespace myclean::telemetry {

std::string volume_entity(const VolumeStatus& volume) {
  std::string entity = volume.mount_points.empty() ? volume.id : volume.mount_points.front();
  if (entity.size() > 1 && entity.ends_with('\\')) {
    entity.pop_back();
  }
  return entity;
}

VolumeCollector::VolumeCollector(const IVolumeInfo& volumes) : volumes_(&volumes) {}

Result<std::vector<Measurement>> VolumeCollector::collect(WallTime now, MonoTime /*monotonic*/) {
  Result<std::vector<VolumeStatus>> volumes = volumes_->volumes();
  if (!volumes) {
    return tl::unexpected(volumes.error());
  }
  std::vector<Measurement> measurements;
  for (const VolumeStatus& volume : *volumes) {
    const std::string entity = volume_entity(volume);
    measurements.push_back(
        measure(metric::volume_size, entity, static_cast<double>(volume.size.count())));
    measurements.push_back(
        measure(metric::volume_free, entity, static_cast<double>(volume.free.count())));
  }
  latest_.publish(VolumeView{.time = now, .volumes = std::move(*volumes)});
  return measurements;
}

}  // namespace myclean::telemetry
