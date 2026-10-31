#pragma once

#include <memory>

#include "core/model/result.hpp"
#include "core/telemetry/perf_counters.hpp"

namespace myclean::platform {

// Counter queries over PDH. Paths are English (PdhAddEnglishCounterW), so they work on any
// display language. Values are read uncapped (PDH_FMT_NOCAP100). Works without elevation.
class PdhCounters final : public telemetry::IPerfCounters {
 public:
  PdhCounters() = default;

  [[nodiscard]] Result<std::unique_ptr<telemetry::ICounterQuery>> open() override;
};

}  // namespace myclean::platform
