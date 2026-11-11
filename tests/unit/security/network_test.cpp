#include "core/security/network.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/events/connections.hpp"
#include "core/events/event_store.hpp"
#include "core/events/events.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/schema.hpp"
#include "fakes/fake_clock.hpp"
#include "security_fakes.hpp"

namespace myclean::security {
namespace {

using namespace std::chrono_literals;
using testing::noon;
using testing::ok;
using testing::TempStore;

events::IpAddress ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
  events::IpAddress address;
  address.bytes[0] = a;
  address.bytes[1] = b;
  address.bytes[2] = c;
  address.bytes[3] = d;
  return address;
}

TEST(SecurityMigrations, AreAdditive) {
  const std::vector<store::MigrationSet> sets = {store::store_migrations(),
                                                 events::event_migrations(), security_migrations()};
  EXPECT_TRUE(ok(store::check_additive(sets)));
}

class NetworkTest : public ::testing::Test {
 protected:
  fakes::FakeClock clock_{noon};
  TempStore store_{clock_};
};

// Spec: "Periodic talker without a window is caught".
TEST_F(NetworkTest, PeriodicWindowlessTalkerComesFromTheHistory) {
  events::EventRecorder recorder(store_.get());
  const auto process = std::make_shared<const events::ProcessStart>(
      events::ProcessStart{.process = {.pid = 4242, .created = noon - 30h},
                           .session = 1,
                           .image = R"(C:\Users\a\AppData\Roaming\beacon.exe)"});
  for (int beat = 0; beat < 96; ++beat) {  // every 15 minutes for 24 hours
    const WallTime time = noon - 24h + (beat * 15min) + 5s;
    recorder.on_network({.time = time,
                         .pid = 4242,
                         .direction = events::Direction::send,
                         .remote_address = ipv4(203, 0, 113, 9),
                         .remote_port = 443,
                         .bytes = 300,
                         .process = process});
    recorder.on_network({.time = time + 1s,
                         .pid = 4242,
                         .direction = events::Direction::receive,
                         .remote_address = ipv4(203, 0, 113, 9),
                         .remote_port = 443,
                         .bytes = 1200,
                         .process = process});
  }
  recorder.on_network({.time = noon - 1h,
                       .pid = 4242,
                       .direction = events::Direction::send,
                       .remote_address = ipv4(127, 0, 0, 1),
                       .remote_port = 9000,
                       .bytes = 99,
                       .process = process});
  recorder.close_all();
  ASSERT_TRUE(ok(store_.get().flush()));

  const Result<std::vector<Traffic>> history = traffic(store_.get(), noon - 24h, noon);
  ASSERT_TRUE(ok(history));
  // The tray reported windows, but not for the beacon; it has exited.
  const WindowFacts windows{.owners = {{1, noon - 50h}}, .reported = true};
  const std::vector<Talker> list = talkers(*history, {}, {}, windows);
  ASSERT_EQ(list.size(), 1U);  // loopback traffic is not a talker
  const Talker& beacon = list[0];
  EXPECT_EQ(beacon.pid, 4242U);
  EXPECT_EQ(beacon.created, noon - 30h);
  EXPECT_EQ(beacon.image, R"(C:\Users\a\AppData\Roaming\beacon.exe)");
  EXPECT_FALSE(beacon.running);
  EXPECT_EQ(beacon.visible_window, false);
  EXPECT_EQ(beacon.sent, 96U * 300U);
  EXPECT_EQ(beacon.received, 96U * 1200U);
  EXPECT_EQ(beacon.first_seen, noon - 24h + 5s);
  EXPECT_EQ(beacon.last_seen, noon - 15min + 6s);
  EXPECT_EQ(beacon.minutes, 96U);
  ASSERT_EQ(beacon.remotes.size(), 1U);
  EXPECT_EQ(beacon.remotes[0].address, "203.0.113.9");
  EXPECT_EQ(beacon.remotes[0].port, 443);
  EXPECT_FALSE(beacon.remotes[0].open_now);
}

TEST(Talkers, CurrentTablesAddOpenConnectionsAndListeners) {
  const std::vector<LiveProcess> live = {
      {.pid = 10, .created = noon - 1h, .image = "app.exe", .session = 1},
      {.pid = 20, .created = noon - 2h, .image = "svc.exe", .session = 0}};
  const std::vector<events::Connection> connections = {
      {.protocol = events::Protocol::tcp,
       .remote_address = ipv4(198, 51, 100, 4),
       .remote_port = 443,
       .state = events::TcpState::established,
       .pid = 10},
      {.protocol = events::Protocol::tcp,
       .local_port = 8080,
       .state = events::TcpState::listen,
       .pid = 20},
      {.protocol = events::Protocol::udp, .local_port = 5353, .pid = 20},
      {.protocol = events::Protocol::tcp,
       .remote_address = ipv4(127, 0, 0, 1),
       .remote_port = 1,
       .state = events::TcpState::established,
       .pid = 10},
      {.protocol = events::Protocol::tcp,
       .remote_address = ipv4(198, 51, 100, 5),
       .remote_port = 80,
       .state = events::TcpState::established,
       .pid = 999}};  // no such process
  const std::vector<Traffic> history = {{.pid = 10,
                                         .created = noon - 1h,
                                         .image = R"(C:\Apps\app.exe)",
                                         .remote_address = "198.51.100.4",
                                         .remote_port = 443,
                                         .sent = 10,
                                         .received = 20,
                                         .first_seen = noon - 10min,
                                         .last_seen = noon - 1min,
                                         .minutes = 3}};
  const WindowFacts windows{.owners = {{10, noon - 1h}}, .reported = true};
  const std::vector<Talker> list = talkers(history, connections, live, windows);
  ASSERT_EQ(list.size(), 2U);
  EXPECT_EQ(list[0].pid, 10U);
  EXPECT_EQ(list[0].image, R"(C:\Apps\app.exe)");  // the history's full path
  EXPECT_TRUE(list[0].running);
  EXPECT_EQ(list[0].visible_window, true);
  ASSERT_EQ(list[0].remotes.size(), 1U);
  EXPECT_TRUE(list[0].remotes[0].open_now);
  EXPECT_EQ(list[0].remotes[0].sent, 10U);
  EXPECT_EQ(list[1].pid, 20U);
  EXPECT_EQ(list[1].visible_window, false);  // a service
  ASSERT_EQ(list[1].listening.size(), 2U);
  EXPECT_EQ(list[1].listening[0].port, 8080);
  EXPECT_EQ(list[1].listening[1].protocol, events::Protocol::udp);
}

TEST(Talkers, WindowUnknownUntilTheTrayReports) {
  const std::vector<LiveProcess> live = {
      {.pid = 10, .created = noon, .image = "a.exe", .session = 1}};
  const std::vector<events::Connection> connections = {{.protocol = events::Protocol::tcp,
                                                        .remote_address = ipv4(192, 0, 2, 1),
                                                        .remote_port = 443,
                                                        .state = events::TcpState::established,
                                                        .pid = 10}};
  const std::vector<Talker> list = talkers({}, connections, live, WindowFacts{});
  ASSERT_EQ(list.size(), 1U);
  EXPECT_FALSE(list[0].visible_window);
}

TEST(LocalAddress, LoopbackAndUnspecified) {
  EXPECT_TRUE(local_address("127.0.0.1"));
  EXPECT_TRUE(local_address("::1"));
  EXPECT_TRUE(local_address("0.0.0.0"));
  EXPECT_TRUE(local_address("::"));
  EXPECT_TRUE(local_address("::ffff:127.0.0.1"));
  EXPECT_FALSE(local_address("192.0.2.1"));
  EXPECT_FALSE(local_address("2001:db8::1"));
}

class FakeResolver final : public IResolver {
 public:
  std::optional<std::string> name(std::string_view address) override {
    ++calls;
    const auto found = names.find(std::string(address));
    return found == names.end() ? std::nullopt : std::optional(found->second);
  }

  std::map<std::string, std::string> names;
  int calls = 0;
};

// Spec: "Reverse DNS enabled by the owner" and "Unresolvable address is still listed".
TEST(ReverseDnsCache, ResolvesOffTheCallersThreadAndCaches) {
  FakeResolver resolver;
  resolver.names = {{"192.0.2.1", "host.example.test"}};
  ReverseDns dns(resolver, 2, 8, std::chrono::hours{1});
  EXPECT_FALSE(dns.lookup("192.0.2.1", noon));  // queued, nothing resolved on this thread
  EXPECT_FALSE(dns.lookup("192.0.2.2", noon));
  EXPECT_FALSE(dns.lookup("192.0.2.1", noon));  // queued once
  EXPECT_EQ(resolver.calls, 0);
  EXPECT_EQ(dns.pending(), 2U);
  EXPECT_EQ(dns.resolve(noon, 10), 2U);
  EXPECT_EQ(dns.lookup("192.0.2.1", noon + 1min), "host.example.test");
  EXPECT_EQ(dns.lookup("192.0.2.2", noon + 1min), "");  // no record: an empty name
  EXPECT_EQ(resolver.calls, 2);
  EXPECT_FALSE(dns.lookup("192.0.2.1", noon + 2h));  // expired: asked again
  EXPECT_EQ(dns.resolve(noon + 2h, 10), 1U);
  EXPECT_FALSE(dns.lookup("192.0.2.3", noon + 2h));
  EXPECT_EQ(dns.resolve(noon + 2h, 10), 1U);
  EXPECT_FALSE(dns.lookup("192.0.2.2", noon + 2h));  // capacity 2: the oldest was dropped
}

TEST(ReverseDnsCache, QueueIsBounded) {
  FakeResolver resolver;
  ReverseDns dns(resolver, 16, 2);
  for (int index = 0; index < 5; ++index) {
    static_cast<void>(dns.lookup("192.0.2." + std::to_string(index), noon));
  }
  EXPECT_EQ(dns.pending(), 2U);
}

TEST(WindowsRecord, AcceptsOnlyProcessIds) {
  EXPECT_EQ(check_windows_record(nlohmann::json{{"pids", {1, 2, 3}}}), "");
  EXPECT_NE(check_windows_record(nlohmann::json{{"pids", nlohmann::json::array()}}), "");
  EXPECT_NE(check_windows_record(nlohmann::json{{"pids", {"x"}}}), "");
  EXPECT_NE(check_windows_record(nlohmann::json{{"pids", {-1}}}), "");
  EXPECT_NE(check_windows_record(nlohmann::json{{"pids", {1}}, {"extra", 1}}), "");
  EXPECT_NE(check_windows_record(nlohmann::json{{"pids", {5000000000LL}}}), "");
}

TEST_F(NetworkTest, WindowLogKeepsReportsAcrossARestart) {
  const std::vector<LiveProcess> live = {{.pid = 10, .created = noon - 1h, .image = "a.exe"},
                                         {.pid = 11, .created = noon - 1h, .image = "b.exe"}};
  {
    WindowLog log(store_.get());
    const std::vector<std::uint32_t> pids = {10, 12};
    log.report(pids, live, noon);
    log.report(pids, live, noon + 1min);  // not written again within the interval
    const WindowFacts facts = log.facts(noon - 1h);
    EXPECT_TRUE(facts.reported);
    EXPECT_TRUE(facts.owns(10, noon - 1h));
    EXPECT_FALSE(facts.owns(11, noon - 1h));
    EXPECT_FALSE(log.facts(noon + 2min).reported);
  }
  EXPECT_EQ(store_.count("SELECT * FROM visible_window"), 1U);
  WindowLog restarted(store_.get());
  ASSERT_TRUE(ok(restarted.load(noon - 24h)));
  const WindowFacts facts = restarted.facts(noon - 1h);
  EXPECT_TRUE(facts.reported);
  EXPECT_TRUE(facts.owns(10, noon - 1h));
}

}  // namespace
}  // namespace myclean::security
