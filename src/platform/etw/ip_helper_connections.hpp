#pragma once

#include <vector>

#include "core/events/connections.hpp"
#include "core/model/result.hpp"

namespace myclean::platform::etw {

// The TCP and UDP tables of IPv4 and IPv6 with the owning PID, from GetExtendedTcpTable and
// GetExtendedUdpTable. Works without elevation.
class IpHelperConnectionTable final : public events::IConnectionTable {
 public:
  [[nodiscard]] Result<std::vector<events::Connection>> snapshot() override;
};

}  // namespace myclean::platform::etw
