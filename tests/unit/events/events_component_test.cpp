#include "core/events/events_component.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/events/connections.hpp"
#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/telemetry/process_table.hpp"
#include "event_fakes.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::events {
namespace {

using namespace std::chrono_literals;
using testing::noon;
using testing::ok;
using testing::RecordingSink;
using testing::start_of;
using testing::TempStore;
using testing::transfer_of;
using Rows = std::vector<std::vector<std::string>>;

std::string seconds(WallTime time) {
  return std::to_string(store::to_unix_seconds(time));
}

// Answers nothing at first, like the trust cache, and hands out what it "verified" since.
class LateSigner final : public ISignerLookup {
 public:
  [[nodiscard]] std::optional<std::string> signer(std::string_view /*image*/) override {
    return std::nullopt;
  }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> take_resolved() override {
    return std::exchange(resolved, {});
  }

  std::vector<std::pair<std::string, std::string>> resolved;
};

class EventsComponentTest : public ::testing::Test {
 protected:
  EventsComponentTest() {
    processes_.set(std::vector<telemetry::ProcessInfo>{
        {.pid = 900, .parent_pid = 4, .created = noon - 1h, .image = "explorer.exe"}});
  }

  fakes::FakeClock& clock() { return clock_; }
  TempStore& store() { return store_; }
  testing::FakeEventSource& source() { return source_; }
  testing::FakeConnectionTable& connections() { return connections_; }
  EventsComponent& component() { return component_; }
  testing::FakeProcessTable& processes() { return processes_; }

 private:
  fakes::FakeClock clock_{noon};
  TempStore store_{clock_};
  testing::FakeEventSource source_;
  testing::FakeConnectionTable connections_;
  testing::FakeProcessTable processes_;
  EventsComponent component_{source_, connections_, processes_};
};

TEST_F(EventsComponentTest, ShortLivedProcessIsDeliveredAndStored) {
  RecordingSink governor;
  component().router().subscribe(governor);
  ASSERT_TRUE(ok(component().start(store().get(), clock())));
  EXPECT_TRUE(component().unavailable().empty());
  ASSERT_NE(source().sink(), nullptr);

  // Recorded sequence: explorer (running before the session) starts cmd.exe, which exits after
  // 120 ms.
  ProcessStart start = start_of(1234, noon + 1s, 900, R"(C:\Windows\System32\cmd.exe)");
  start.command_line = "cmd.exe /c exit 3";
  source().sink()->on_start(start);
  source().sink()->on_stop(ProcessStop{
      .process = start.process, .exited = noon + 1s + 120ms, .exit_code = 3, .image = "cmd.exe"});

  ASSERT_EQ(governor.starts.size(), 1U);
  ASSERT_EQ(governor.stops.size(), 1U);
  EXPECT_EQ(governor.starts[0].parent, (ProcessKey{.pid = 900, .created = noon - 1h}));
  EXPECT_EQ(governor.stops[0].exit_code, 3U);
  EXPECT_EQ(store().rows("SELECT pid, parent_time, image FROM proc_start"),
            (Rows{{"1234", seconds(noon - 1h), R"(C:\Windows\System32\cmd.exe)"}}));

  component().stop();
  EXPECT_EQ(source().stops(), 1);
  component().router().unsubscribe(governor);
}

TEST(EventsComponent, TheTickGivesStartsTheSignerVerifiedAfterThey) {
  const fakes::FakeClock clock{noon};
  TempStore store{clock};
  testing::FakeEventSource source;
  testing::FakeConnectionTable connections;
  testing::FakeProcessTable processes;
  LateSigner signers;
  EventsComponent component{source, connections, processes, &signers};
  ASSERT_TRUE(ok(component.start(store.get(), clock)));
  const std::string tool = R"(C:\Program Files\PowerToys\PowerToys.exe)";
  source.sink()->on_start(start_of(1234, noon + 1s, 4, tool));
  source.sink()->on_stop(ProcessStop{.process = ProcessKey{.pid = 1234, .created = noon + 1s},
                                     .exited = noon + 2s,
                                     .image = "PowerToys.exe"});

  signers.resolved = {{tool, "Microsoft Corporation"}};
  component.tick();
  component.stop();

  EXPECT_EQ(store.rows("SELECT pid, signer FROM proc_start"),
            (Rows{{"1234", "Microsoft Corporation"}}));
  EXPECT_EQ(store.rows("SELECT pid, image FROM proc_stop"), (Rows{{"1234", "PowerToys.exe"}}));
}

TEST_F(EventsComponentTest, NetworkMinutesAreWrittenByTheTickAndOnStop) {
  ASSERT_TRUE(ok(component().start(store().get(), clock())));
  source().sink()->on_network(transfer_of(900, noon + 5s, Direction::send, 4096));
  clock().advance(1min + 2s);
  component().tick();
  EXPECT_EQ(store().rows("SELECT image, sent FROM net_minute"), (Rows{{"explorer.exe", "4096"}}));

  source().sink()->on_network(transfer_of(900, noon + 1min + 3s, Direction::receive, 10));
  component().stop();
  EXPECT_EQ(store().rows("SELECT received FROM net_minute ORDER BY time"), (Rows{{"0"}, {"10"}}));
}

TEST_F(EventsComponentTest, UnavailableEtwIsReportedWithoutFailingTheStart) {
  source().fail_with(Error{.code = Errc::access_denied, .message = "needs SYSTEM"});
  processes().set(fail(Errc::io_error, "table unreadable"));

  ASSERT_TRUE(ok(component().start(store().get(), clock())));
  const std::vector<Error> unavailable = component().unavailable();
  ASSERT_EQ(unavailable.size(), 2U);
  EXPECT_EQ(unavailable[0].code, Errc::access_denied);
  EXPECT_EQ(unavailable[0].message, "ETW unavailable: needs SYSTEM");
  EXPECT_EQ(unavailable[1].message, "process seed: table unreadable");
  ASSERT_NE(component().connections(), nullptr);  // snapshots work regardless

  component().stop();
  EXPECT_EQ(source().stops(), 0);  // never started
}

TEST_F(EventsComponentTest, ConnectionsAreSnapshottedEveryMinute) {
  connections().set(std::vector<Connection>{
      {.protocol = Protocol::tcp, .local_port = 135, .state = TcpState::listen, .pid = 1100}});
  ASSERT_TRUE(ok(component().start(store().get(), clock())));
  ASSERT_NE(component().connections(), nullptr);
  EXPECT_EQ(component().connections()->time, noon);
  ASSERT_EQ(component().connections()->connections.size(), 1U);
  EXPECT_EQ(component().connections()->connections[0].pid, 1100U);

  const int after_start = connections().snapshots();
  clock().advance(30s);
  component().tick();
  EXPECT_EQ(connections().snapshots(), after_start);

  connections().set(fail(Errc::access_denied, "denied"));
  clock().advance(30s);
  component().tick();
  EXPECT_EQ(connections().snapshots(), after_start + 1);
  EXPECT_EQ(component().connections()->time, noon);  // the older view stays
  ASSERT_EQ(component().unavailable().size(), 1U);
  EXPECT_EQ(component().unavailable()[0].message, "connections: denied");

  connections().set(std::vector<Connection>{});
  clock().advance(1min);
  component().tick();
  EXPECT_TRUE(component().unavailable().empty());
  EXPECT_TRUE(component().connections()->connections.empty());
}

TEST_F(EventsComponentTest, SecondStartIsRefused) {
  ASSERT_TRUE(ok(component().start(store().get(), clock())));
  const Result<void> again = component().start(store().get(), clock());
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error().code, Errc::invalid_state);
}

TEST_F(EventsComponentTest, OwnsTheEventsSchema) {
  EXPECT_EQ(component().name(), "events");
  ASSERT_EQ(component().migrations().size(), 1U);
  EXPECT_EQ(component().migrations()[0].component, "events");
}

}  // namespace
}  // namespace myclean::events
