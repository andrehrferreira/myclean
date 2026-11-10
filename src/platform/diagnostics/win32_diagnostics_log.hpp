#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/model/result.hpp"
#include "core/startup/diagnostics.hpp"

namespace myclean::platform {

// startup::IDiagnosticsLog over Microsoft-Windows-Diagnostics-Performance/Operational (EvtQuery
// for events 100 and 101, newest first, at most max_events, each rendered as XML). The channel's
// DACL lets only SYSTEM, elevated administrators, LocalService, NetworkService and service
// logons read it: elsewhere events() fails with access_denied.
class Win32DiagnosticsLog final : public startup::IDiagnosticsLog {
 public:
  static constexpr std::uint32_t max_events = 2'000;

  Win32DiagnosticsLog() = default;

  [[nodiscard]] Result<std::vector<std::string>> events() const override;
};

}  // namespace myclean::platform
