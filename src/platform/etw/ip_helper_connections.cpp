#include "platform/etw/ip_helper_connections.hpp"

// clang-format off: winsock2.h must precede windows.h, which iphlpapi.h includes.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string_view>
#include <vector>

#include "core/events/connections.hpp"
#include "core/events/events.hpp"
#include "core/model/result.hpp"

namespace myclean::platform::etw {

namespace {

using events::Connection;
using events::IpAddress;
using events::Protocol;
using events::TcpState;

// Ports are in network byte order in the low 16 bits.
std::uint16_t port_of(DWORD network_order) {
  const auto port = static_cast<std::uint16_t>(network_order);
  return static_cast<std::uint16_t>((port >> 8U) | (port << 8U));
}

IpAddress ipv4_of(DWORD address) {
  IpAddress result;
  std::memcpy(result.bytes.data(), &address, sizeof(address));
  return result;
}

IpAddress ipv6_of(const UCHAR (&address)[16]) {  // NOLINT(*-avoid-c-arrays): MIB row layout
  IpAddress result{.v6 = true};
  std::memcpy(result.bytes.data(), static_cast<const UCHAR*>(address), result.bytes.size());
  return result;
}

TcpState state_of(DWORD state) {
  return state >= MIB_TCP_STATE_CLOSED && state <= MIB_TCP_STATE_DELETE_TCB
             ? static_cast<TcpState>(state)
             : TcpState::none;
}

// Calls `fetch(buffer, &size)` until the buffer is large enough; tables grow between calls.
template <typename Fetch>
Result<std::vector<std::byte>> fetch_table(std::string_view name, Fetch fetch) {
  std::vector<std::byte> buffer;
  DWORD size = 0;
  DWORD status = ERROR_INSUFFICIENT_BUFFER;
  for (int attempt = 0; attempt < 4 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
    buffer.resize(size);
    status = fetch(buffer.empty() ? nullptr : buffer.data(), &size);
  }
  if (status != NO_ERROR) {
    return fail(Errc::io_error, std::format("{} failed with error {}", name, status));
  }
  return buffer;
}

// The rows of a MIB_*TABLE_OWNER_PID: a DWORD count followed by `Table::table`.
template <typename Table, typename Row>
std::vector<Row> rows_of(std::span<const std::byte> buffer) {
  std::vector<Row> rows;
  DWORD count = 0;
  if (buffer.size() < sizeof(count)) {
    return rows;
  }
  std::memcpy(&count, buffer.data(), sizeof(count));
  const std::size_t first = offsetof(Table, table);
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t offset = first + (index * sizeof(Row));
    if (offset + sizeof(Row) > buffer.size()) {
      break;
    }
    Row row{};
    std::memcpy(&row, buffer.subspan(offset, sizeof(Row)).data(), sizeof(Row));
    rows.push_back(row);
  }
  return rows;
}

template <typename Table, typename Row>
Result<std::vector<Row>> tcp_rows(ULONG family) {
  Result<std::vector<std::byte>> buffer =
      fetch_table("GetExtendedTcpTable", [family](void* table, DWORD* size) {
        return ::GetExtendedTcpTable(table, size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
      });
  if (!buffer) {
    return tl::unexpected(buffer.error());
  }
  return rows_of<Table, Row>(*buffer);
}

template <typename Table, typename Row>
Result<std::vector<Row>> udp_rows(ULONG family) {
  Result<std::vector<std::byte>> buffer =
      fetch_table("GetExtendedUdpTable", [family](void* table, DWORD* size) {
        return ::GetExtendedUdpTable(table, size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
      });
  if (!buffer) {
    return tl::unexpected(buffer.error());
  }
  return rows_of<Table, Row>(*buffer);
}

}  // namespace

Result<std::vector<Connection>> IpHelperConnectionTable::snapshot() {
  const auto tcp4 = tcp_rows<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(AF_INET);
  const auto tcp6 = tcp_rows<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>(AF_INET6);
  const auto udp4 = udp_rows<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>(AF_INET);
  const auto udp6 = udp_rows<MIB_UDP6TABLE_OWNER_PID, MIB_UDP6ROW_OWNER_PID>(AF_INET6);
  for (const Error* error : {tcp4 ? nullptr : &tcp4.error(), tcp6 ? nullptr : &tcp6.error(),
                             udp4 ? nullptr : &udp4.error(), udp6 ? nullptr : &udp6.error()}) {
    if (error != nullptr) {
      return tl::unexpected(*error);
    }
  }

  std::vector<Connection> connections;
  connections.reserve(tcp4->size() + tcp6->size() + udp4->size() + udp6->size());
  for (const MIB_TCPROW_OWNER_PID& row : *tcp4) {
    connections.push_back({.protocol = Protocol::tcp,
                           .local_address = ipv4_of(row.dwLocalAddr),
                           .local_port = port_of(row.dwLocalPort),
                           .remote_address = ipv4_of(row.dwRemoteAddr),
                           .remote_port = port_of(row.dwRemotePort),
                           .state = state_of(row.dwState),
                           .pid = row.dwOwningPid});
  }
  for (const MIB_TCP6ROW_OWNER_PID& row : *tcp6) {
    connections.push_back({.protocol = Protocol::tcp,
                           .local_address = ipv6_of(row.ucLocalAddr),
                           .local_port = port_of(row.dwLocalPort),
                           .remote_address = ipv6_of(row.ucRemoteAddr),
                           .remote_port = port_of(row.dwRemotePort),
                           .state = state_of(row.dwState),
                           .pid = row.dwOwningPid});
  }
  for (const MIB_UDPROW_OWNER_PID& row : *udp4) {
    connections.push_back({.protocol = Protocol::udp,
                           .local_address = ipv4_of(row.dwLocalAddr),
                           .local_port = port_of(row.dwLocalPort),
                           .pid = row.dwOwningPid});
  }
  for (const MIB_UDP6ROW_OWNER_PID& row : *udp6) {
    connections.push_back({.protocol = Protocol::udp,
                           .local_address = ipv6_of(row.ucLocalAddr),
                           .local_port = port_of(row.dwLocalPort),
                           .pid = row.dwOwningPid});
  }
  return connections;
}

}  // namespace myclean::platform::etw
