#include "platform/etw/ip_helper_connections.hpp"

// clang-format off: winsock2.h must precede windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "core/events/connections.hpp"
#include "core/events/events.hpp"
#include "core/model/result.hpp"

namespace myclean::platform::etw {
namespace {

// A UDP socket bound to a free loopback port; loopback binds raise no firewall prompt.
class LoopbackSocket {
 public:
  LoopbackSocket()
      : started_(start_winsock()), socket_(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
    sockaddr_in address{.sin_family = AF_INET, .sin_port = 0};
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API
    auto* const generic = reinterpret_cast<sockaddr*>(&address);
    int length = sizeof(address);
    if (::bind(socket_, generic, length) == 0 && ::getsockname(socket_, generic, &length) == 0) {
      port_ = ::ntohs(address.sin_port);
    }
  }
  LoopbackSocket(const LoopbackSocket&) = delete;
  LoopbackSocket& operator=(const LoopbackSocket&) = delete;
  LoopbackSocket(LoopbackSocket&&) = delete;
  LoopbackSocket& operator=(LoopbackSocket&&) = delete;
  ~LoopbackSocket() {
    if (socket_ != INVALID_SOCKET) {
      ::closesocket(socket_);
    }
    if (started_) {
      ::WSACleanup();
    }
  }

  [[nodiscard]] std::uint16_t port() const { return port_; }

 private:
  static bool start_winsock() {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }

  bool started_ = false;
  SOCKET socket_ = INVALID_SOCKET;
  std::uint16_t port_ = 0;
};

TEST(IpHelperConnectionTable, ListsThisProcessUdpEndpointWithItsPid) {
  const LoopbackSocket socket;
  ASSERT_NE(socket.port(), 0U);

  IpHelperConnectionTable table;
  const Result<std::vector<events::Connection>> connections = table.snapshot();
  ASSERT_TRUE(connections.has_value()) << connections.error().message;

  const auto mine = std::ranges::find_if(*connections, [&](const events::Connection& connection) {
    return connection.protocol == events::Protocol::udp && connection.local_port == socket.port();
  });
  ASSERT_NE(mine, connections->end());
  EXPECT_EQ(mine->pid, ::GetCurrentProcessId());
  EXPECT_EQ(events::to_string(mine->local_address), "127.0.0.1");
  EXPECT_FALSE(mine->remote_address.has_value());
  EXPECT_EQ(mine->state, events::TcpState::none);
}

TEST(IpHelperConnectionTable, HasTcpListenersOfOtherProcesses) {
  IpHelperConnectionTable table;
  const Result<std::vector<events::Connection>> connections = table.snapshot();
  ASSERT_TRUE(connections.has_value()) << connections.error().message;
  // Every Windows machine listens on the RPC endpoint mapper (135) in svchost.
  const auto listener = std::ranges::find_if(*connections, [](const events::Connection& c) {
    return c.protocol == events::Protocol::tcp && c.state == events::TcpState::listen &&
           c.local_port == 135;
  });
  ASSERT_NE(listener, connections->end());
  EXPECT_NE(listener->pid, ::GetCurrentProcessId());
  EXPECT_GT(listener->pid, 4U);
  ASSERT_TRUE(listener->remote_address.has_value());
}

}  // namespace
}  // namespace myclean::platform::etw
