#include "core/events/events.hpp"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/events/connections.hpp"

namespace myclean::events {
namespace {

IpAddress v6(std::array<std::uint8_t, 16> bytes) {
  return IpAddress{.bytes = bytes, .v6 = true};
}

TEST(IpAddress, FormatsIpv4) {
  EXPECT_EQ(to_string(IpAddress{.bytes = {192, 0, 2, 1}}), "192.0.2.1");
  EXPECT_EQ(to_string(IpAddress{}), "0.0.0.0");
}

TEST(IpAddress, FormatsIpv6InRfc5952Form) {
  EXPECT_EQ(to_string(v6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})),
            "2001:db8::1");
  EXPECT_EQ(to_string(v6({})), "::");
  EXPECT_EQ(to_string(v6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})), "::1");
  EXPECT_EQ(to_string(v6({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})), "fe80::");
  // A single zero group is not compressed; the first of two equal runs is.
  EXPECT_EQ(to_string(v6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1})),
            "2001:db8:0:1:1:1:1:1");
  EXPECT_EQ(to_string(v6({0x20, 0x01, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1})),
            "2001::1:0:0:1:1");
  // The longest run wins.
  EXPECT_EQ(to_string(v6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0})),
            "2001:db8:0:0:1::");
}

TEST(Protocol, RoundTripsItsTextForm) {
  EXPECT_EQ(to_string(Protocol::tcp), "tcp");
  EXPECT_EQ(parse_protocol("udp"), Protocol::udp);
  EXPECT_FALSE(parse_protocol("icmp").has_value());
}

TEST(TcpState, HasTextForms) {
  EXPECT_EQ(to_string(TcpState::established), "established");
  EXPECT_EQ(to_string(TcpState::delete_tcb), "delete_tcb");
  EXPECT_EQ(to_string(TcpState::none), "none");
}

}  // namespace
}  // namespace myclean::events
