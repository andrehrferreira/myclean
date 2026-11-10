#include "core/startup/boot_times.hpp"

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
#include "core/model/units.hpp"
#include "core/startup/diagnostics.hpp"
#include "core/startup/impact.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "startup_fakes.hpp"

namespace myclean::startup {
namespace {

using namespace std::chrono_literals;

const WallTime day = std::chrono::sys_days{std::chrono::November / 9 / 2026};

BootRecord boot_at(WallTime start, std::chrono::milliseconds logon) {
  return BootRecord{.start = start,
                    .boot = logon + 20s,
                    .main_path = 25s,
                    .post_boot = logon - 5s,
                    .logon = logon};
}

TEST(BootLogonTest, ABootLeadsToTheFirstMeasuredLogonBeforeTheNextBoot) {
  const std::vector<BootRecord> boots = {boot_at(day + 8h, 30s), boot_at(day + 20h, 30s),
                                         boot_at(day + 30h, 30s)};
  const std::vector<WallTime> logons = {day + 7h, day + 8h + 40s, day + 9h, day + 31h};
  EXPECT_EQ(boot_logon(boots, 0, logons), day + 8h + 40s);
  EXPECT_EQ(boot_logon(boots, 1, logons), std::nullopt);  // no logon measured after it
  EXPECT_EQ(boot_logon(boots, 2, logons), day + 31h);
  EXPECT_EQ(boot_logon(boots, 3, logons), std::nullopt);
  EXPECT_EQ(boot_logon(boots, 0, {}), std::nullopt);
}

TEST(ByImpactTest, OrdersByDiskBytesThenCpu) {
  const std::vector<ImageImpact> ordered =
      by_impact({{.image = "a.exe", .cpu_seconds = 9.0, .io_bytes = 10},
                 {.image = "b.exe", .cpu_seconds = 1.0, .io_bytes = 900},
                 {.image = "c.exe", .cpu_seconds = 2.0, .io_bytes = 10},
                 {.image = "d.exe", .cpu_seconds = 9.0, .io_bytes = 10}});
  ASSERT_EQ(ordered.size(), 4U);
  EXPECT_EQ(ordered[0].image, "b.exe");
  EXPECT_EQ(ordered[1].image, "a.exe");
  EXPECT_EQ(ordered[2].image, "d.exe");
  EXPECT_EQ(ordered[3].image, "c.exe");
}

TEST(MedianTest, TakesTheMiddleBootAndLogon) {
  EXPECT_EQ(median_of({}), std::nullopt);
  const std::vector<BootRecord> odd = {boot_at(day, 30s), boot_at(day + 1h, 70s),
                                       boot_at(day + 2h, 40s)};
  EXPECT_EQ(median_of(odd), (DurationMedian{.boot = 60s, .logon = 40s, .boots = 3}));
  const std::vector<BootRecord> even = {boot_at(day, 30s), boot_at(day + 1h, 40s)};
  EXPECT_EQ(median_of(even), (DurationMedian{.boot = 55s, .logon = 35s, .boots = 2}));
}

TEST(BootsAroundTest, TakesTheLatestBootsBeforeAndTheFirstAfter) {
  std::vector<BootRecord> boots;
  boots.reserve(8);
  for (int index = 0; index < 8; ++index) {
    boots.push_back(boot_at(day + std::chrono::hours{index}, 30s));
  }
  const WallTime change = day + 3h;
  const std::span<const BootRecord> before = boots_before(boots, change, 2);
  ASSERT_EQ(before.size(), 2U);
  EXPECT_EQ(before[0].start, day + 1h);
  EXPECT_EQ(before[1].start, day + 2h);
  const std::span<const BootRecord> after = boots_after(boots, change, 2);
  ASSERT_EQ(after.size(), 2U);
  EXPECT_EQ(after[0].start, day + 3h);
  EXPECT_EQ(boots_before(boots, change, 10).size(), 3U);
  EXPECT_EQ(boots_after(boots, change, 10).size(), 5U);
  EXPECT_TRUE(boots_before(boots, day, 5).empty());
  EXPECT_TRUE(boots_after(boots, day + 9h, 5).empty());
}

class BootStoreTest : public ::testing::Test {
 public:
  BootStoreTest()
      : folder_(std::filesystem::temp_directory_path() /
                ("myclean-boot-" +
                 std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
    const std::array sets{impact_migrations()};
    store_ = store::Store::open(folder_ / "myclean.db", clock_, sets).value();
  }
  BootStoreTest(const BootStoreTest&) = delete;
  BootStoreTest& operator=(const BootStoreTest&) = delete;
  BootStoreTest(BootStoreTest&&) = delete;
  BootStoreTest& operator=(BootStoreTest&&) = delete;
  ~BootStoreTest() override {
    store_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

 protected:
  store::Store& store() { return *store_; }

 private:
  std::filesystem::path folder_;
  fakes::FakeClock clock_{day};
  std::unique_ptr<store::Store> store_;
};

// Startup spec "Boot and Logon Time Tracking": each boot Windows measured is recorded once, and
// stays when the event log has rolled over.
TEST_F(BootStoreTest, RecordsEachBootOnceFromTheLog) {
  testing::FakeDiagnosticsLog log;
  log.xml = {
      testing::boot_event_xml("2026-11-09T08:00:00.5000000Z", 60'000, 30'000, 20'000, 2'000, 3'000),
      testing::delay_event_xml("OneDrive.exe", 2'000, "2026-11-09T08:03:00Z"),
      testing::boot_event_xml("2026-11-08T08:00:00Z", 50'000, 30'000, 15'000, 2'000, 3'000)};
  ASSERT_TRUE(record_boot_events(store(), log));
  ASSERT_TRUE(record_boot_events(store(), log));  // the same boots again: replaced, not added
  log.xml.resize(1);                              // the log rolled over
  ASSERT_TRUE(record_boot_events(store(), log));
  const Result<std::vector<BootRecord>> boots = stored_boots(store());
  ASSERT_TRUE(boots) << boots.error().message;
  ASSERT_EQ(boots->size(), 2U);
  EXPECT_EQ((*boots)[0].start, day - 16h);
  EXPECT_EQ((*boots)[0].logon, 20s);
  EXPECT_EQ((*boots)[1].start, day + 8h);  // whole seconds, as stored
  EXPECT_EQ((*boots)[1].boot, 60s);
  EXPECT_EQ((*boots)[1].main_path, 30s);
  EXPECT_EQ((*boots)[1].post_boot, 20s);
  EXPECT_EQ((*boots)[1].logon, 25s);

  log.error = Error{.code = Errc::access_denied, .message = "denied"};
  EXPECT_EQ(record_boot_events(store(), log).error().code, Errc::access_denied);
}

TEST_F(BootStoreTest, ReadsTheMeasuredLogonsAndTheirImpact) {
  const std::vector<ImageImpact> impact = {{.image = "a.exe", .cpu_seconds = 1.5, .io_bytes = 10},
                                           {.image = "b.exe", .cpu_seconds = 0.5, .io_bytes = 99}};
  ASSERT_TRUE(record_logon(store(), day + 8h, impact));
  ASSERT_TRUE(record_logon(store(), day + 2h, {}));
  EXPECT_EQ(stored_logons(store()).value(), (std::vector<WallTime>{day + 2h, day + 8h}));
  EXPECT_EQ(stored_logon_impact(store(), day + 8h).value(), impact);
  EXPECT_TRUE(stored_logon_impact(store(), day + 2h).value().empty());
}

}  // namespace
}  // namespace myclean::startup
