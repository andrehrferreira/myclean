#include "core/startup/impact.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/startup/startup.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::startup {
namespace {

using namespace std::chrono_literals;

const WallTime logon = std::chrono::sys_days{std::chrono::November / 9 / 2026} + 8h;

TEST(CommandImageTest, FindsTheProgramOfACommand) {
  // A raw string with a quote inside stays out of macro arguments: MSVC's preprocessor splits it.
  const std::string quoted = R"("C:\Users\o\AppData\Local\Discord\Update.exe" --processStart)";
  EXPECT_EQ(command_image(quoted), "update.exe");
  EXPECT_EQ(command_image(R"(C:\Program Files\Steam\steam.exe -silent)"), "steam.exe");
  EXPECT_EQ(command_image(R"(C:\Windows\system32\rundll32.exe x.dll,Run)"), "rundll32.exe");
  EXPECT_EQ(command_image(R"(  C:\tools\agent.cmd /quiet)"), "agent.cmd");
  EXPECT_EQ(command_image(R"(C:\Program Files\Mongo\bin\mongod.EXE)"), "mongod.exe");
  EXPECT_EQ(command_image(""), "");
}

// Discord.exe was started by Update.exe, which has exited: its use counts for update.exe, the
// program of the Run value. A service already running counts with its use since the logon only.
TEST(LogonImpactTest, AttributesEachProcessToTheProgramThatStartedItsChain) {
  const WallTime update_start = logon + 5s;
  const WallTime discord_start = logon + 7s;
  const WallTime renderer_start = logon + 9s;
  const WallTime service_start = logon - 30s;
  const std::vector<Start> starts = {
      {.pid = 100,
       .time = update_start,
       .parent_pid = 4,
       .parent_time = logon - 1h,
       .image = R"(C:\Users\o\AppData\Local\Discord\Update.exe)"},
      {.pid = 101,
       .time = discord_start,
       .parent_pid = 100,
       .parent_time = update_start,
       .image = R"(C:\Users\o\AppData\Local\Discord\app-1.0\Discord.exe)"},
      {.pid = 102,
       .time = renderer_start,
       .parent_pid = 101,
       .parent_time = discord_start,
       .image = "Discord.exe"},
      // Started after the window: not counted.
      {.pid = 103, .time = logon + 3min, .parent_pid = 4, .image = "late.exe"}};
  const std::vector<Usage> baseline = {
      {.pid = 50, .created = service_start, .image = "mongod.exe", .cpu = 2s, .io_bytes = 1000}};
  const std::vector<Usage> end = {
      {.pid = 101, .created = discord_start, .image = "Discord.exe", .cpu = 3s, .io_bytes = 500},
      {.pid = 102, .created = renderer_start, .image = "Discord.exe", .cpu = 1s, .io_bytes = 100},
      {.pid = 50, .created = service_start, .image = "mongod.exe", .cpu = 5s, .io_bytes = 4000},
      // Running before the logon but missing from the baseline: left out.
      {.pid = 60, .created = logon - 10s, .image = "other.exe", .cpu = 9s, .io_bytes = 9},
      {.pid = 103, .created = logon + 3min, .image = "late.exe", .cpu = 1s, .io_bytes = 1}};
  const std::vector<ImageImpact> impacts = logon_impact(logon, starts, baseline, end);
  ASSERT_EQ(impacts.size(), 2U);
  EXPECT_EQ(impacts[0], (ImageImpact{.image = "mongod.exe", .cpu_seconds = 3.0, .io_bytes = 3000}));
  EXPECT_EQ(impacts[1], (ImageImpact{.image = "update.exe", .cpu_seconds = 4.0, .io_bytes = 600}));
}

TEST(ItemImpactTest, MedianOverLogonsElseBootDelaysElseUnknown) {
  const std::vector<Item> items = {
      {.id = "run:user:Discord", .command = R"("C:\D\Update.exe" --processStart Discord.exe)"},
      {.id = "run:user:Steam", .command = R"(C:\Steam\steam.exe -silent)"},
      {.id = "service:machine:x", .command = R"(C:\x\x.exe)"}};
  const std::vector<std::vector<ImageImpact>> history = {
      {{.image = "update.exe", .cpu_seconds = 4.0, .io_bytes = 600}},
      {{.image = "update.exe", .cpu_seconds = 2.0, .io_bytes = 200}},
      {}};
  const std::vector<BootDelay> delays = {{.image = "steam.exe", .delay_seconds = 1.5},
                                         {.image = "steam.exe", .delay_seconds = 3.0},
                                         {.image = "update.exe", .delay_seconds = 9.0}};
  const std::vector<std::optional<ItemImpact>> impacts = item_impacts(items, history, delays);
  ASSERT_EQ(impacts.size(), 3U);
  ASSERT_TRUE(impacts[0].has_value());
  ASSERT_TRUE(impacts[1].has_value());
  const ItemImpact measured = impacts[0].value_or(ItemImpact{});
  EXPECT_EQ(measured.source, ImpactSource::myclean);
  EXPECT_EQ(measured.cpu_seconds, 2.0);  // median of 4, 2 and 0 (absent at one logon)
  EXPECT_EQ(measured.io_bytes, 200U);
  EXPECT_EQ(measured.logons, 3U);
  const ItemImpact delayed = impacts[1].value_or(ItemImpact{});
  EXPECT_EQ(delayed.source, ImpactSource::diagnostics);
  EXPECT_EQ(delayed.delay_seconds, 3.0);
  EXPECT_FALSE(impacts[2].has_value());
}

TEST(ImpactMigrations, AreAdditive) {
  const std::array sets{impact_migrations()};
  EXPECT_TRUE(store::check_additive(sets));
}

class LogonStoreTest : public ::testing::Test {
 public:
  LogonStoreTest()
      : folder_(std::filesystem::temp_directory_path() /
                ("myclean-startup-" +
                 std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    const std::array sets{impact_migrations()};
    store_ = store::Store::open(folder_ / "myclean.db", clock_, sets).value();
  }
  LogonStoreTest(const LogonStoreTest&) = delete;
  LogonStoreTest& operator=(const LogonStoreTest&) = delete;
  LogonStoreTest(LogonStoreTest&&) = delete;
  LogonStoreTest& operator=(LogonStoreTest&&) = delete;
  ~LogonStoreTest() override {
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

 protected:
  store::Store& store() { return *store_; }

 private:
  std::filesystem::path folder_;
  fakes::FakeClock clock_{logon};
  std::unique_ptr<store::Store> store_;
};

TEST_F(LogonStoreTest, RecordsLogonsAndReadsTheLatestFirst) {
  EXPECT_FALSE(logon_recorded(store(), logon).value());
  const std::vector<ImageImpact> first = {{.image = "a.exe", .cpu_seconds = 1.5, .io_bytes = 10}};
  const std::vector<ImageImpact> second = {{.image = "a.exe", .cpu_seconds = 2.5, .io_bytes = 20},
                                           {.image = "b.exe", .cpu_seconds = 0.5, .io_bytes = 5}};
  ASSERT_TRUE(record_logon(store(), logon, first));
  ASSERT_TRUE(record_logon(store(), logon + 24h, second));
  ASSERT_TRUE(record_logon(store(), logon + 48h, {}));  // a logon with nothing measured counts
  EXPECT_TRUE(logon_recorded(store(), logon).value());

  const Result<std::vector<std::vector<ImageImpact>>> history = logon_history(store(), 2);
  ASSERT_TRUE(history) << history.error().message;
  ASSERT_EQ(history->size(), 2U);
  EXPECT_TRUE((*history)[0].empty());
  EXPECT_EQ((*history)[1], second);

  ASSERT_TRUE(record_logon(store(), logon, second));  // replaced
  EXPECT_EQ(logon_history(store(), 3).value().at(2), second);
}

// A logon is measured once: baseline at the first tick, end sample after the window, impact
// stored once the starts are flushed.
TEST_F(LogonStoreTest, TheRecorderMeasuresEachLogonOnce) {
  LogonRecorder recorder(store());
  int queries = 0;
  const StartSource starts = [&](WallTime from, WallTime to) -> Result<std::vector<Start>> {
    ++queries;
    EXPECT_EQ(from, logon);
    EXPECT_EQ(to, logon + logon_window);
    return std::vector<Start>{{.pid = 7, .time = logon + 3s, .parent_pid = 1, .image = "app.exe"}};
  };
  std::vector<Usage> sample = {{.pid = 7, .created = logon + 3s, .image = "app.exe"}};
  ASSERT_TRUE(recorder.tick(logon - 1min, std::nullopt, sample, starts));
  ASSERT_TRUE(recorder.tick(logon + 2s, logon, sample, starts));
  sample[0].cpu = 4s;
  sample[0].io_bytes = 40;
  ASSERT_TRUE(recorder.tick(logon + logon_window, logon, sample, starts));  // end sample
  sample[0].cpu = 60s;  // after the window: not counted
  ASSERT_TRUE(recorder.tick(logon + logon_window + 5s, logon, sample, starts));
  EXPECT_EQ(queries, 0);
  EXPECT_FALSE(logon_recorded(store(), logon).value());
  ASSERT_TRUE(
      recorder.tick(logon + logon_window + LogonRecorder::flush_delay, logon, sample, starts));
  EXPECT_EQ(queries, 1);
  EXPECT_EQ(logon_history(store(), 1).value().at(0),
            (std::vector<ImageImpact>{{.image = "app.exe", .cpu_seconds = 4.0, .io_bytes = 40}}));
  ASSERT_TRUE(recorder.tick(logon + 10min, logon, sample, starts));
  EXPECT_EQ(queries, 1);

  // The service restarts: a logon stored already is not measured again.
  LogonRecorder restarted(store());
  ASSERT_TRUE(restarted.tick(logon + 1s, logon, sample, starts));
  ASSERT_TRUE(restarted.tick(logon + 1h, logon, sample, starts));
  EXPECT_EQ(queries, 1);
}

TEST_F(LogonStoreTest, ALogonFirstSeenLateIsNotMeasured) {
  LogonRecorder recorder(store());
  int queries = 0;
  const StartSource starts = [&](WallTime, WallTime) -> Result<std::vector<Start>> {
    ++queries;
    return std::vector<Start>{};
  };
  const WallTime late = logon + LogonRecorder::baseline_grace + 1s;
  ASSERT_TRUE(recorder.tick(late, logon, {}, starts));
  ASSERT_TRUE(recorder.tick(logon + 1h, logon, {}, starts));
  EXPECT_EQ(queries, 0);
  EXPECT_FALSE(logon_recorded(store(), logon).value());
}

}  // namespace
}  // namespace myclean::startup
