#include "platform/etw/etw_records.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>

#include "core/events/events.hpp"
#include "core/model/units.hpp"

namespace myclean::platform::etw {

namespace {

constexpr USHORT process_start_event = 1;
constexpr USHORT process_stop_event = 2;

struct NetworkEvent {
  USHORT id;
  events::Protocol protocol;
  events::Direction direction;
};
constexpr std::array<NetworkEvent, 8> network_events = {{
    {.id = 10, .protocol = events::Protocol::tcp, .direction = events::Direction::send},  // IPv4
    {.id = 11, .protocol = events::Protocol::tcp, .direction = events::Direction::receive},
    {.id = 26, .protocol = events::Protocol::tcp, .direction = events::Direction::send},  // IPv6
    {.id = 27, .protocol = events::Protocol::tcp, .direction = events::Direction::receive},
    {.id = 42, .protocol = events::Protocol::udp, .direction = events::Direction::send},  // IPv4
    {.id = 43, .protocol = events::Protocol::udp, .direction = events::Direction::receive},
    {.id = 58, .protocol = events::Protocol::udp, .direction = events::Direction::send},  // IPv6
    {.id = 59, .protocol = events::Protocol::udp, .direction = events::Direction::receive},
}};

// FILETIME units: 100 ns since 1601-01-01.
using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;

WallTime wall_time_of(std::int64_t file_time) {
  constexpr std::int64_t unix_epoch = 116'444'736'000'000'000;
  return WallTime{std::chrono::duration_cast<WallTime::duration>(Ticks{file_time - unix_epoch})};
}

WallTime wall_time_of(const FILETIME& file_time) {
  return wall_time_of(static_cast<std::int64_t>((std::uint64_t{file_time.dwHighDateTime} << 32U) |
                                                file_time.dwLowDateTime));
}

std::string utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
  return result;
}

events::IpAddress address_of(const krabs::ip_address& address) {
  // krabs::ip_address is a union of the two forms; v4 is the first four bytes either way.
  events::IpAddress result{.v6 = address.is_ipv6};
  std::memcpy(result.bytes.data(), &address, address.is_ipv6 ? result.bytes.size() : sizeof(DWORD));
  return result;
}

// Ports are in network byte order.
std::uint16_t port_of(std::uint16_t network_order) {
  return static_cast<std::uint16_t>((network_order >> 8U) | (network_order << 8U));
}

}  // namespace

std::optional<events::ProcessStart> process_start_of(const EVENT_RECORD& record,
                                                     const krabs::schema_locator& schemas) {
  if (record.EventHeader.EventDescriptor.Id != process_start_event) {
    return std::nullopt;
  }
  try {
    const krabs::schema schema(record, schemas);
    krabs::parser parser(schema);
    return events::ProcessStart{
        .process = {.pid = parser.parse<std::uint32_t>(L"ProcessID"),
                    .created = wall_time_of(parser.parse<FILETIME>(L"CreateTime"))},
        .parent_pid = parser.parse<std::uint32_t>(L"ParentProcessID"),
        .session = parser.parse<std::uint32_t>(L"SessionID"),
        .image = utf8(parser.parse<std::wstring>(L"ImageName"))};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<events::ProcessStop> process_stop_of(const EVENT_RECORD& record,
                                                   const krabs::schema_locator& schemas) {
  if (record.EventHeader.EventDescriptor.Id != process_stop_event) {
    return std::nullopt;
  }
  try {
    const krabs::schema schema(record, schemas);
    krabs::parser parser(schema);
    return events::ProcessStop{
        .process = {.pid = parser.parse<std::uint32_t>(L"ProcessID"),
                    .created = wall_time_of(parser.parse<FILETIME>(L"CreateTime"))},
        .exited = wall_time_of(parser.parse<FILETIME>(L"ExitTime")),
        .exit_code = parser.parse<std::uint32_t>(L"ExitCode"),
        .image = parser.parse<std::string>(L"ImageName")};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<events::NetworkTransfer> network_transfer_of(const EVENT_RECORD& record,
                                                           const krabs::schema_locator& schemas) {
  const USHORT id = record.EventHeader.EventDescriptor.Id;
  const auto kind = std::ranges::find(network_events, id, &NetworkEvent::id);
  if (kind == network_events.end()) {
    return std::nullopt;
  }
  try {
    const krabs::schema schema(record, schemas);
    krabs::parser parser(schema);
    // daddr and dport are the remote end in both directions.
    return events::NetworkTransfer{
        .time = wall_time_of(record.EventHeader.TimeStamp.QuadPart),
        .pid = parser.parse<std::uint32_t>(L"PID"),
        .protocol = kind->protocol,
        .direction = kind->direction,
        .remote_address = address_of(parser.parse<krabs::ip_address>(L"daddr")),
        .remote_port = port_of(parser.parse<std::uint16_t>(L"dport")),
        .bytes = parser.parse<std::uint32_t>(L"size")};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace myclean::platform::etw
