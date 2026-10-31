#include "core/drives/actions.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/action.hpp"
#include "core/model/result.hpp"
#include "core/model/series.hpp"
#include "core/model/units.hpp"
#include "core/store/store.hpp"
#include "core/telemetry/metrics.hpp"
#include "drives_fakes.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::drives {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::FakeDriveStates;
using testing::FakeIdle;
using testing::FakeLayout;
using testing::FakeLog;
using testing::FakePower;
using testing::FakeReader;
using testing::FakeRunner;
using testing::file;
using testing::hdd;
using testing::optimized;
using testing::scattered;
using testing::ssd;

const WallTime now = std::chrono::sys_days{std::chrono::October / 31 / 2026} + 3h;

struct MaintenanceTest : public ::testing::Test {
  MaintenanceTest() {
    drives.states = {ssd("D:\\", 4), hdd("K:\\", 0)};
    idle.percent = {{0, 99.0}, {4, 98.5}};
    events.logged = std::vector<OptimizerEvent>{
        optimized(now - 45 * 24h, "SSD AR (D:)", OptimizationKind::retrim)};
    reader.listings[L"K:\\"] = {file("a.bin", (1_GiB).count())};
    layout.runs[L"K:\\a.bin"] = scattered(200);
  }

  [[nodiscard]] MaintenanceTools tools() {
    return MaintenanceTools{
        .analysis = {.reader = &reader, .layout = &layout, .log = &events, .clock = &clock},
        .drives = &drives,
        .idle = &idle,
        .power = &power,
        .runner = &runner};
  }

  // The SSD retrimmed now, as the optimizer logs it.
  void log_retrim_now() {
    events.logged->push_back(optimized(now, "SSD AR (D:)", OptimizationKind::retrim));
  }

  FakeDriveStates drives;
  FakeIdle idle;
  FakePower power;
  FakeRunner runner;
  FakeReader reader;
  FakeLayout layout;
  FakeLog events;
  fakes::FakeClock clock{now};
};

TEST(MaintenanceCommand, IsWindowsDefragAtItsDefaultLowPriority) {
  EXPECT_EQ(maintenance_command(Maintenance::retrim, L"D:\\").text(), "defrag D: /L");
  EXPECT_EQ(maintenance_command(Maintenance::defrag, L"K:\\").text(), "defrag K: /D");
}

TEST_F(MaintenanceTest, DescribesRetrimAsAutomaticAndDefragAsAsk) {
  const ActionInfo retrim = RetrimVolume{L"D:\\", tools()}.describe();
  EXPECT_EQ(retrim.what, "Retrim D:\\");
  EXPECT_EQ(retrim.tier, Tier::automatic);
  EXPECT_EQ(retrim.context, ExecutionContext::service);
  EXPECT_NE(retrim.why.find("defrag D: /L"), std::string::npos);
  const ActionInfo defrag = DefragVolume{L"K:\\", tools()}.describe();
  EXPECT_EQ(defrag.tier, Tier::ask);
  EXPECT_NE(defrag.why.find("defrag K: /D"), std::string::npos);
}

TEST_F(MaintenanceTest, RetrimsAnIdleHealthySsdAndVerifiesFromTheLog) {
  RetrimVolume action{L"D:\\", tools()};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  ASSERT_EQ(preview->items.size(), 1U);
  ASSERT_TRUE(action.analysis().has_value());
  EXPECT_EQ(action.analysis().value_or(VolumeAnalysis{}).recommended, Maintenance::retrim);

  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  ASSERT_EQ(runner.ran.size(), 1U);
  EXPECT_EQ(runner.ran.front().text(), "defrag D: /L");
  EXPECT_FALSE(undo->reversible());

  log_retrim_now();
  const Result<Verification> verification = action.verify(*undo);
  ASSERT_TRUE(verification.has_value()) << verification.error().message;
  EXPECT_EQ(verification->metric, "days_since_retrim");
  EXPECT_DOUBLE_EQ(verification->before, 45.0);
  EXPECT_DOUBLE_EQ(verification->after, 0.0);
  EXPECT_EQ(verification->effect, Effect::better);

  const Result<void> undone = action.undo(*undo);
  ASSERT_FALSE(undone.has_value());
  EXPECT_EQ(undone.error().code, Errc::not_reversible);
}

TEST_F(MaintenanceTest, DefragmentsAnIdleHealthyHdd) {
  DefragVolume action{L"K:\\", tools()};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value()) << preview.error().message;
  const Result<Undo> undo = action.apply(*preview);
  ASSERT_TRUE(undo.has_value()) << undo.error().message;
  ASSERT_EQ(runner.ran.size(), 1U);
  EXPECT_EQ(runner.ran.front().text(), "defrag K: /D");

  layout.runs[L"K:\\a.bin"] = scattered(1);
  const Result<Verification> verification = action.verify(*undo);
  ASSERT_TRUE(verification.has_value()) << verification.error().message;
  EXPECT_EQ(verification->metric, "fragmented_percent");
  EXPECT_DOUBLE_EQ(verification->before, 100.0);
  EXPECT_DOUBLE_EQ(verification->after, 0.0);
  EXPECT_EQ(verification->effect, Effect::better);
}

TEST_F(MaintenanceTest, AnSsdIsNeverDefragmentedAndAnHddNeverRetrimmed) {
  DefragVolume defrag{L"D:\\", tools()};
  const Result<Preview> refused = defrag.preview();
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, Errc::refused);
  EXPECT_NE(refused.error().message.find("never defragmented"), std::string::npos);
  // Even with a preview made up by hand, apply refuses.
  const Result<Undo> applied = defrag.apply(Preview{.items = {PlanItem{.path = L"D:\\"}}});
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error().code, Errc::refused);

  RetrimVolume retrim{L"K:\\", tools()};
  EXPECT_EQ(retrim.preview().error().code, Errc::refused);
  EXPECT_TRUE(runner.ran.empty());
}

TEST_F(MaintenanceTest, ABusyDiskDefersTheRun) {
  idle.percent[4] = 40.0;
  RetrimVolume action{L"D:\\", tools()};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());
  const Result<Undo> applied = action.apply(*preview);
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error().code, Errc::refused);
  EXPECT_NE(applied.error().message.find("disk 4 was 40.0 % idle"), std::string::npos);
  EXPECT_TRUE(runner.ran.empty());

  idle.percent[4] = std::nullopt;
  EXPECT_NE(action.apply(*preview).error().message.find("too little idle-time history"),
            std::string::npos);
  EXPECT_TRUE(runner.ran.empty());
}

TEST_F(MaintenanceTest, ADegradedDriveOrBatteryPowerIsRefused) {
  DefragVolume action{L"K:\\", tools()};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());

  drives.states[1].healthy = false;
  drives.states[1].health_note = "disk 0 health is warning; 1 new pending sector";
  const Result<Undo> degraded = action.apply(*preview);
  ASSERT_FALSE(degraded.has_value());
  EXPECT_NE(degraded.error().message.find("pending sector"), std::string::npos);

  drives.states[1].healthy = true;
  power.battery = true;
  const Result<Undo> battery = action.apply(*preview);
  ASSERT_FALSE(battery.has_value());
  EXPECT_NE(battery.error().message.find("battery"), std::string::npos);
  EXPECT_TRUE(runner.ran.empty());
}

TEST_F(MaintenanceTest, AFailingDefragRunIsAnError) {
  runner.exit_code = 2;
  RetrimVolume action{L"D:\\", tools()};
  const Result<Preview> preview = action.preview();
  ASSERT_TRUE(preview.has_value());
  const Result<Undo> applied = action.apply(*preview);
  ASSERT_FALSE(applied.has_value());
  EXPECT_EQ(applied.error().code, Errc::io_error);
  EXPECT_NE(applied.error().message.find("exited with 2"), std::string::npos);
}

struct StoreDiskIdleTest : public ::testing::Test {
  StoreDiskIdleTest() {
    folder = std::filesystem::temp_directory_path() / "myclean-drives-idle";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    Result<std::unique_ptr<store::Store>> opened = store::Store::open(folder / "myclean.db", clock);
    if (opened) {
      db = std::move(*opened);
    }
  }
  StoreDiskIdleTest(const StoreDiskIdleTest&) = delete;
  StoreDiskIdleTest& operator=(const StoreDiskIdleTest&) = delete;
  StoreDiskIdleTest(StoreDiskIdleTest&&) = delete;
  StoreDiskIdleTest& operator=(StoreDiskIdleTest&&) = delete;
  ~StoreDiskIdleTest() override {
    db.reset();
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
  }

  // One disk.idle sample a second for `seconds`, ending now.
  void record(std::uint32_t disk, int seconds, double percent) const {
    for (int second = seconds; second > 0; --second) {
      db->record(SeriesKey{.metric = std::string(telemetry::metric::disk_idle),
                           .entity = std::to_string(disk)},
                 Sample{.time = now - std::chrono::seconds{second}, .value = percent});
    }
  }

  fakes::FakeClock clock{now};
  std::filesystem::path folder;
  std::unique_ptr<store::Store> db;
};

TEST_F(StoreDiskIdleTest, MeanOfTheWindowAndNothingWithoutEnoughSamples) {
  ASSERT_NE(db, nullptr);
  record(0, 600, 95.0);
  record(1, 100, 99.0);
  const StoreDiskIdle idle{*db, clock};
  const Result<std::optional<double>> full = idle.idle_percent(0, 10min);
  ASSERT_TRUE(full.has_value()) << full.error().message;
  ASSERT_TRUE(full->has_value());
  EXPECT_DOUBLE_EQ(**full, 95.0);
  const Result<std::optional<double>> sparse = idle.idle_percent(1, 10min);
  ASSERT_TRUE(sparse.has_value());
  EXPECT_FALSE(sparse->has_value());
  EXPECT_FALSE(idle.idle_percent(2, 10min)->has_value());
}

}  // namespace
}  // namespace myclean::drives
