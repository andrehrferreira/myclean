// Recorded-event fixtures: EVENT_RECORDs synthesized with krabsetw's test builder from the
// providers' manifests on this machine, then converted like the live session converts them.
#include "platform/etw/etw_records.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <krabs/testing/record_builder.hpp>

#include "core/events/events.hpp"
#include "core/model/units.hpp"

namespace myclean::platform::etw {
namespace {

using namespace std::chrono_literals;

// 2026-10-31 12:00:00 UTC.
const WallTime noon =
    std::chrono::sys_days{std::chrono::October / 31 / 2026} + std::chrono::hours{12};

std::int64_t file_time_ticks(WallTime time) {
  constexpr std::int64_t unix_epoch = 116'444'736'000'000'000;
  return std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
             time.time_since_epoch())
             .count() +
         unix_epoch;
}

FILETIME file_time(WallTime time) {
  const auto ticks = static_cast<std::uint64_t>(file_time_ticks(time));
  return FILETIME{.dwLowDateTime = static_cast<DWORD>(ticks),
                  .dwHighDateTime = static_cast<DWORD>(ticks >> 32U)};
}

const krabs::guid process_provider{kernel_process_provider};
const krabs::guid network_provider{kernel_network_provider};

// The converted event; a failure and an empty event when the record did not convert.
template <typename Event>
Event converted(const std::optional<Event>& event) {
  if (!event) {
    ADD_FAILURE() << "the record did not convert";
    return Event{};
  }
  return *event;
}

TEST(EtwRecords, ProcessStartOfWindows11) {
  krabs::testing::record_builder builder(process_provider, 1, 4, 1, 4);
  // S-1-16-8192, the medium mandatory label: a SID with one sub-authority fills SID exactly.
  const SID medium{.Revision = SID_REVISION,
                   .SubAuthorityCount = 1,
                   .IdentifierAuthority = SECURITY_MANDATORY_LABEL_AUTHORITY,
                   .SubAuthority = {SECURITY_MANDATORY_MEDIUM_RID}};
  builder.add_properties()(L"ProcessID", std::uint32_t{4321})(
      L"CreateTime", file_time(noon + 250ms))(L"ParentProcessID", std::uint32_t{1200})(
      L"SessionID", std::uint32_t{1})(L"MandatoryLabel", medium)(
      L"ImageName", std::wstring(LR"(\Device\HarddiskVolume3\Windows\System32\cmd.exe)"));
  const krabs::testing::synth_record record = builder.pack_incomplete();
  const krabs::schema_locator schemas;

  const events::ProcessStart start = converted(process_start_of(record, schemas));
  EXPECT_EQ(start.process.pid, 4321U);
  EXPECT_EQ(start.process.created, noon + 250ms);
  EXPECT_EQ(start.parent_pid, 1200U);
  EXPECT_EQ(start.session, 1U);
  EXPECT_EQ(start.image, R"(\Device\HarddiskVolume3\Windows\System32\cmd.exe)");
  EXPECT_FALSE(start.parent.has_value());
  EXPECT_FALSE(start.command_line.has_value());
  EXPECT_FALSE(process_stop_of(record, schemas).has_value());
  EXPECT_FALSE(network_transfer_of(record, schemas).has_value());
}

TEST(EtwRecords, ProcessStartOfTheFirstEventVersion) {
  krabs::testing::record_builder builder(process_provider, 1, 0, 1, 4);
  builder.add_properties()(L"ProcessID", std::uint32_t{88})(L"CreateTime", file_time(noon))(
      L"ParentProcessID", std::uint32_t{4})(L"SessionID", std::uint32_t{0})(
      L"ImageName", std::wstring(LR"(\Device\HarddiskVolume3\Windows\System32\smss.exe)"));
  const krabs::testing::synth_record record = builder.pack();
  const krabs::schema_locator schemas;

  const events::ProcessStart start = converted(process_start_of(record, schemas));
  EXPECT_EQ(start.process.pid, 88U);
  EXPECT_EQ(start.image, R"(\Device\HarddiskVolume3\Windows\System32\smss.exe)");
}

TEST(EtwRecords, ProcessStop) {
  krabs::testing::record_builder builder(process_provider, 2, 2, 2, 4);
  builder.add_properties()(L"ProcessID", std::uint32_t{4321})(
      L"CreateTime", file_time(noon + 250ms))(L"ExitTime", file_time(noon + 400ms))(
      L"ExitCode", std::uint32_t{3})(L"ImageName", std::string("cmd.exe"));
  const krabs::testing::synth_record record = builder.pack_incomplete();
  const krabs::schema_locator schemas;

  const events::ProcessStop stop = converted(process_stop_of(record, schemas));
  EXPECT_EQ(stop.process.pid, 4321U);
  EXPECT_EQ(stop.process.created, noon + 250ms);
  EXPECT_EQ(stop.exited, noon + 400ms);
  EXPECT_EQ(stop.exit_code, 3U);
  EXPECT_EQ(stop.image, "cmd.exe");
  EXPECT_FALSE(process_start_of(record, schemas).has_value());
}

TEST(EtwRecords, TcpSendOverIpv4) {
  krabs::testing::record_builder builder(network_provider, 10, 0, 10, 4);
  builder.header().TimeStamp.QuadPart = file_time_ticks(noon + 2s);
  const std::uint32_t remote = 0x0702'00C0;  // 192.0.2.7 in network byte order
  builder.add_properties()(L"PID", std::uint32_t{4321})(L"size", std::uint32_t{1460})(
      L"daddr", remote)(L"dport", std::uint16_t{0xBB01});  // port 443
  const krabs::testing::synth_record record = builder.pack_incomplete();
  const krabs::schema_locator schemas;

  const events::NetworkTransfer transfer = converted(network_transfer_of(record, schemas));
  EXPECT_EQ(transfer.time, noon + 2s);
  EXPECT_EQ(transfer.pid, 4321U);
  EXPECT_EQ(transfer.protocol, events::Protocol::tcp);
  EXPECT_EQ(transfer.direction, events::Direction::send);
  EXPECT_EQ(events::to_string(transfer.remote_address), "192.0.2.7");
  EXPECT_EQ(transfer.remote_port, 443U);
  EXPECT_EQ(transfer.bytes, 1460U);
}

TEST(EtwRecords, TcpReceiveOverIpv6) {
  krabs::testing::record_builder builder(network_provider, 27, 0, 11, 4);
  const std::array<BYTE, 16> remote = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  builder.add_properties()(L"PID", std::uint32_t{77})(L"size", std::uint32_t{65536})(
      L"daddr", krabs::binary(remote.data(), remote.size()))(
      L"saddr", krabs::binary(remote.data(), remote.size()))(L"dport",
                                                             std::uint16_t{0x5000});  // port 80
  const krabs::testing::synth_record record = builder.pack_incomplete();
  const krabs::schema_locator schemas;

  const events::NetworkTransfer transfer = converted(network_transfer_of(record, schemas));
  EXPECT_EQ(transfer.direction, events::Direction::receive);
  EXPECT_EQ(events::to_string(transfer.remote_address), "2001:db8::1");
  EXPECT_EQ(transfer.remote_port, 80U);
  EXPECT_EQ(transfer.bytes, 65536U);
}

TEST(EtwRecords, UdpSendOverIpv4) {
  krabs::testing::record_builder builder(network_provider, 42, 0, 42, 4);
  builder.add_properties()(L"PID", std::uint32_t{9})(L"size", std::uint32_t{512})(
      L"daddr", std::uint32_t{0x0101'A8C0})(L"dport", std::uint16_t{0x3500});  // 192.168.1.1:53
  const krabs::testing::synth_record record = builder.pack_incomplete();
  const krabs::schema_locator schemas;

  const events::NetworkTransfer transfer = converted(network_transfer_of(record, schemas));
  EXPECT_EQ(transfer.protocol, events::Protocol::udp);
  EXPECT_EQ(events::to_string(transfer.remote_address), "192.168.1.1");
  EXPECT_EQ(transfer.remote_port, 53U);
}

TEST(EtwRecords, OtherNetworkEventsAreIgnored) {
  // Event 12 is a TCP connect: no bytes moved.
  const krabs::testing::record_builder builder(network_provider, 12, 0, 12, 4);
  const krabs::testing::synth_record record = builder.pack_incomplete();
  const krabs::schema_locator schemas;
  EXPECT_FALSE(network_transfer_of(record, schemas).has_value());
}

}  // namespace
}  // namespace myclean::platform::etw
