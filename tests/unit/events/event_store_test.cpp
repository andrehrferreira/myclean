#include "core/events/event_store.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/events/events.hpp"
#include "core/model/result.hpp"
#include "core/store/migrations.hpp"
#include "core/store/schema.hpp"
#include "event_fakes.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::events {
namespace {

using namespace std::chrono_literals;
using testing::noon;
using testing::ok;
using testing::start_of;
using testing::TempStore;
using testing::transfer_of;
using Rows = std::vector<std::vector<std::string>>;

constexpr std::uint64_t mib = std::uint64_t{1} << 20U;

std::string seconds(WallTime time) {
  return std::to_string(store::to_unix_seconds(time));
}

TEST(EventMigrations, AreAdditive) {
  const std::vector<store::MigrationSet> sets = {store::store_migrations(), event_migrations()};
  EXPECT_TRUE(ok(store::check_additive(sets)));
}

TEST(CommandLineHash, IsFnv1a64) {
  EXPECT_EQ(static_cast<std::uint64_t>(command_line_hash("")), 0xcbf29ce484222325ULL);
  EXPECT_EQ(static_cast<std::uint64_t>(command_line_hash("a")), 0xaf63dc4c8601ec8cULL);
  EXPECT_NE(command_line_hash("cl.exe /c a.cpp"), command_line_hash("cl.exe /c b.cpp"));
}

class EventRecorderTest : public ::testing::Test {
 protected:
  TempStore& store() { return store_; }
  EventRecorder& recorder() { return recorder_; }

 private:
  fakes::FakeClock clock_{noon};
  TempStore store_{clock_};
  EventRecorder recorder_{store_.get()};
};

TEST_F(EventRecorderTest, StoresEveryStartWithTheCommandLineHash) {
  ProcessStart child = start_of(200, noon + 5s, 100, R"(C:\Windows\System32\cmd.exe)");
  child.parent = ProcessKey{.pid = 100, .created = noon};
  child.command_line = "cmd.exe /c exit 3";
  child.signer = "Microsoft Windows";
  recorder().on_start(start_of(100, noon, 4, R"(C:\Windows\explorer.exe)"));
  recorder().on_start(child);

  const Rows rows = store().rows(
      "SELECT time, pid, parent_pid, parent_time, session, image, command_line_hash, signer "
      "FROM proc_start ORDER BY time");
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0], (std::vector<std::string>{seconds(noon), "100", "4", "NULL", "1",
                                               R"(C:\Windows\explorer.exe)", "NULL", "NULL"}));
  EXPECT_EQ(
      rows[1],
      (std::vector<std::string>{
          seconds(noon + 5s), "200", "100", seconds(noon), "1", R"(C:\Windows\System32\cmd.exe)",
          std::to_string(command_line_hash("cmd.exe /c exit 3")), "Microsoft Windows"}));
}

TEST_F(EventRecorderTest, UploadOfTenMegabytesShowsInThatMinute) {
  NetworkTransfer chunk = transfer_of(100, noon + 3s, Direction::send, mib);
  chunk.process =
      std::make_shared<const ProcessStart>(start_of(100, noon - 1h, 4, R"(C:\Tools\backup.exe)"));
  for (int index = 0; index < 10; ++index) {
    chunk.time = noon + std::chrono::seconds{3 + (5 * index)};
    recorder().on_network(chunk);
  }
  recorder().on_network(transfer_of(100, noon + 59s, Direction::receive, 2000));

  recorder().close_minutes(noon + 1min + 1s);  // the minute is not closed yet
  EXPECT_TRUE(store().rows("SELECT time FROM net_minute").empty());
  recorder().close_minutes(noon + 1min + 2s);

  const Rows rows = store().rows(
      "SELECT time, pid, process_time, image, protocol, remote_address, remote_port, sent, "
      "received, first_seen, last_seen FROM net_minute ORDER BY process_time");
  ASSERT_EQ(rows.size(), 2U);
  // The receive came from an unknown PID instance.
  EXPECT_EQ(rows[0], (std::vector<std::string>{seconds(noon), "100", "NULL", "NULL", "tcp",
                                               "192.0.2.7", "443", "0", "2000", seconds(noon + 59s),
                                               seconds(noon + 59s)}));
  EXPECT_EQ(rows[1], (std::vector<std::string>{seconds(noon), "100", seconds(noon - 1h),
                                               R"(C:\Tools\backup.exe)", "tcp", "192.0.2.7", "443",
                                               std::to_string(10 * mib), "0", seconds(noon + 3s),
                                               seconds(noon + 48s)}));

  const Result<std::vector<ProcessNetwork>> totals =
      network_by_process(store().get(), noon, noon + 1min);
  ASSERT_TRUE(ok(totals));
  ASSERT_EQ(totals->size(), 2U);
  EXPECT_EQ((*totals)[0].image, R"(C:\Tools\backup.exe)");
  EXPECT_EQ((*totals)[0].created, noon - 1h);
  EXPECT_EQ((*totals)[0].sent, 10 * mib);
  EXPECT_EQ((*totals)[1].received, 2000U);
  EXPECT_FALSE((*totals)[1].created.has_value());
}

TEST_F(EventRecorderTest, LateTransferAddsARowThatQueriesSum) {
  recorder().on_network(transfer_of(100, noon + 10s, Direction::send, 500));
  recorder().close_minutes(noon + 2min);
  recorder().on_network(transfer_of(100, noon + 50s, Direction::send, 300));
  recorder().close_all();

  EXPECT_EQ(store().rows("SELECT time FROM net_minute").size(), 2U);
  const Result<std::vector<ProcessNetwork>> totals =
      network_by_process(store().get(), noon, noon + 1min);
  ASSERT_TRUE(ok(totals));
  ASSERT_EQ(totals->size(), 1U);
  EXPECT_EQ((*totals)[0].sent, 800U);
}

TEST_F(EventRecorderTest, SeparatesMinutesProtocolsAndEndpoints) {
  NetworkTransfer udp = transfer_of(100, noon + 10s, Direction::send, 1);
  udp.protocol = Protocol::udp;
  NetworkTransfer other_port = transfer_of(100, noon + 10s, Direction::send, 1);
  other_port.remote_port = 80;
  recorder().on_network(transfer_of(100, noon + 10s, Direction::send, 1));
  recorder().on_network(transfer_of(100, noon + 70s, Direction::send, 1));
  recorder().on_network(udp);
  recorder().on_network(other_port);
  recorder().close_all();

  EXPECT_EQ(store().rows("SELECT time FROM net_minute").size(), 4U);
}

TEST_F(EventRecorderTest, RetentionKeepsTwoYearsOfStartsAndThirtyDaysOfMinutes) {
  ASSERT_TRUE(ok(add_event_retention(store().get())));
  recorder().on_start(start_of(1, noon - std::chrono::days{731}, 4, R"(C:\old.exe)"));
  recorder().on_start(start_of(2, noon - std::chrono::days{31}, 4, R"(C:\recent.exe)"));
  recorder().on_network(transfer_of(1, noon - std::chrono::days{31}, Direction::send, 1));
  recorder().on_network(transfer_of(1, noon - std::chrono::days{29}, Direction::send, 1));
  recorder().close_all();
  ASSERT_TRUE(ok(store().get().flush()));

  ASSERT_TRUE(ok(store().get().run_retention()));
  EXPECT_EQ(store().rows("SELECT pid FROM proc_start"), (Rows{{"2"}}));
  EXPECT_EQ(store().rows("SELECT time FROM net_minute"),
            (Rows{{seconds(noon - std::chrono::days{29})}}));
}

}  // namespace
}  // namespace myclean::events
