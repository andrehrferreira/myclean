#pragma once

#include "core/model/result.hpp"
#include "core/telemetry/memory_status.hpp"

namespace myclean::platform {

// Memory status from one GetPerformanceInfo call, so every field is from the same instant.
class Win32MemoryStatus final : public telemetry::IMemoryStatus {
 public:
  Win32MemoryStatus() = default;

  [[nodiscard]] Result<telemetry::MemoryStatus> read() const override;
};

}  // namespace myclean::platform
