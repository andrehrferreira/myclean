#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"

namespace myclean::events {

// MIB_TCP_STATE order; `none` for UDP endpoints, which have no state.
enum class TcpState {
  none,
  closed,
  listen,
  syn_sent,
  syn_received,
  established,
  fin_wait1,
  fin_wait2,
  close_wait,
  closing,
  last_ack,
  time_wait,
  delete_tcb,
};
[[nodiscard]] std::string_view to_string(TcpState state);

// One TCP connection or listener, or one UDP endpoint, with the process that owns it.
struct Connection {
  Protocol protocol = Protocol::tcp;
  IpAddress local_address;
  std::uint16_t local_port = 0;
  std::optional<IpAddress> remote_address;  // nullopt for UDP
  std::uint16_t remote_port = 0;
  TcpState state = TcpState::none;
  std::uint32_t pid = 0;
};

struct ConnectionView {
  WallTime time;
  std::vector<Connection> connections;
};

// The current TCP and UDP endpoints, IPv4 and IPv6. Implemented by src/platform/etw.
class IConnectionTable {
 public:
  virtual ~IConnectionTable() = default;
  IConnectionTable(const IConnectionTable&) = delete;
  IConnectionTable& operator=(const IConnectionTable&) = delete;
  IConnectionTable(IConnectionTable&&) = delete;
  IConnectionTable& operator=(IConnectionTable&&) = delete;

  [[nodiscard]] virtual Result<std::vector<Connection>> snapshot() = 0;

 protected:
  IConnectionTable() = default;
};

}  // namespace myclean::events
