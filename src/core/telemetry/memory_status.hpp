#pragma once

#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::telemetry {

// System memory at one instant; every field comes from the same reading.
struct MemoryStatus {
  Bytes total;  // physical memory usable by Windows
  Bytes available;
  Bytes commit;
  Bytes commit_limit;
  Bytes pool_paged;
  Bytes pool_nonpaged;
};

// Implemented by src/platform/perf.
class IMemoryStatus {
 public:
  virtual ~IMemoryStatus() = default;
  IMemoryStatus(const IMemoryStatus&) = delete;
  IMemoryStatus& operator=(const IMemoryStatus&) = delete;
  IMemoryStatus(IMemoryStatus&&) = delete;
  IMemoryStatus& operator=(IMemoryStatus&&) = delete;

  [[nodiscard]] virtual Result<MemoryStatus> read() const = 0;

 protected:
  IMemoryStatus() = default;
};

}  // namespace myclean::telemetry
