#pragma once

#include <vector>

#include "core/health/reading.hpp"
#include "core/model/result.hpp"

namespace myclean::health {

// Reads the raw health data of every physical disk. Implemented by src/platform/disk_health.
class IDiskHealthSource {
 public:
  virtual ~IDiskHealthSource() = default;
  IDiskHealthSource(const IDiskHealthSource&) = delete;
  IDiskHealthSource& operator=(const IDiskHealthSource&) = delete;
  IDiskHealthSource(IDiskHealthSource&&) = delete;
  IDiskHealthSource& operator=(IDiskHealthSource&&) = delete;

  // One probe per physical disk present. A disk whose health cannot be read is still listed,
  // with the reason in DriveProbe::problem. Fails only when the disks cannot be enumerated.
  [[nodiscard]] virtual Result<std::vector<DriveProbe>> probe() const = 0;

 protected:
  IDiskHealthSource() = default;
};

}  // namespace myclean::health
