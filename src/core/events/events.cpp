#include "core/events/events.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "core/events/connections.hpp"
#include "core/model/enum_names.hpp"

namespace myclean::events {

namespace {

constexpr std::array<std::string_view, 2> protocol_names = {"tcp", "udp"};
constexpr std::array<std::string_view, 13> tcp_state_names = {
    "none",      "closed",     "listen",  "syn_sent", "syn_received", "established", "fin_wait1",
    "fin_wait2", "close_wait", "closing", "last_ack", "time_wait",    "delete_tcb"};

constexpr std::size_t ipv6_groups = 8;

std::string ipv6_text(const std::array<std::uint8_t, 16>& bytes) {
  std::array<unsigned, ipv6_groups> groups{};
  for (std::size_t index = 0; index < ipv6_groups; ++index) {
    groups.at(index) = (unsigned{bytes.at(2 * index)} << 8U) | bytes.at((2 * index) + 1);
  }
  // RFC 5952: the longest run of two or more zero groups (the first on a tie) becomes "::".
  std::size_t best_start = ipv6_groups;
  std::size_t best_length = 1;
  for (std::size_t start = 0; start < ipv6_groups;) {
    std::size_t end = start;
    while (end < ipv6_groups && groups.at(end) == 0) {
      ++end;
    }
    if (end - start > best_length) {
      best_start = start;
      best_length = end - start;
    }
    start = end == start ? start + 1 : end;
  }
  std::string text;
  for (std::size_t index = 0; index < ipv6_groups; ++index) {
    if (index == best_start) {
      text += "::";
      index += best_length - 1;
      continue;
    }
    if (!text.empty() && !text.ends_with(':')) {
      text += ':';
    }
    text += std::format("{:x}", groups.at(index));
  }
  return text;
}

}  // namespace

std::string_view to_string(Protocol protocol) {
  return detail::enum_name(protocol_names, protocol);
}

std::optional<Protocol> parse_protocol(std::string_view text) {
  return detail::enum_from_name<Protocol>(protocol_names, text);
}

std::string_view to_string(TcpState state) {
  return detail::enum_name(tcp_state_names, state);
}

std::string to_string(const IpAddress& address) {
  if (address.v6) {
    return ipv6_text(address.bytes);
  }
  return std::format("{}.{}.{}.{}", address.bytes[0], address.bytes[1], address.bytes[2],
                     address.bytes[3]);
}

}  // namespace myclean::events
