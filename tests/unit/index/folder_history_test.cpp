#include "core/index/folder_history.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/index/volume_index.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::index {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;

namespace fs = std::filesystem;

FolderSize folder(const fs::path& path, Bytes size_on_disk) {
  return FolderSize{.path = path, .size_on_disk = size_on_disk, .size = size_on_disk};
}

std::vector<FolderSize> volume(Bytes temp, Bytes windows) {
  const Bytes users = 99_GiB + temp;
  return {folder(R"(C:\)", users + windows),
          folder(R"(C:\Users)", users),
          folder(R"(C:\Users\bo)", 89_GiB + temp),
          folder(R"(C:\Users\bo\AppData)", 39_GiB + temp),
          folder(R"(C:\Users\bo\AppData\Local)", 29_GiB + temp),
          folder(R"(C:\Users\bo\AppData\Local\Temp)", temp),
          folder(R"(C:\Windows)", windows)};
}

// The user's Temp folder grew 6 GB, and every folder above it with it.
TEST(Growth, ListsTheFolderWhereTheGrowthHappenedNotItsParents) {
  const std::vector<FolderGrowth> grown = growth(volume(1_GiB, 40_GiB), volume(7_GiB, 40_GiB), 10);
  ASSERT_EQ(grown.size(), 1U);
  EXPECT_EQ(
      grown[0],
      (FolderGrowth{.path = R"(C:\Users\bo\AppData\Local\Temp)", .before = 1_GiB, .after = 7_GiB}));
}

TEST(Growth, KeepsAParentWhenNoSubfolderAccountsFor90PercentOfItsGrowth) {
  const std::vector<FolderGrowth> grown = growth(volume(1_GiB, 40_GiB), volume(7_GiB, 41_GiB), 10);
  ASSERT_EQ(grown.size(), 3U);
  // C:\ grew 7 GB, of which C:\Users accounts for 6 GB (86 %).
  EXPECT_EQ(grown[0].path, fs::path{"C:\\"});
  EXPECT_EQ(grown[1].path, fs::path{R"(C:\Users\bo\AppData\Local\Temp)"});
  EXPECT_EQ(grown[2].path, fs::path{"C:\\Windows"});
}

TEST(Growth, LeavesShrinkingAndUnchangedFoldersOut) {
  const std::vector<FolderSize> before{folder("D:\\", 10_GiB), folder("D:\\a", 4_GiB),
                                       folder("D:\\b", 3_GiB)};
  const std::vector<FolderSize> now{folder("D:\\", 8_GiB), folder("D:\\a", 2_GiB),
                                    folder("D:\\b", 3_GiB)};
  EXPECT_TRUE(growth(before, now, 10).empty());
}

TEST(Growth, ListsAParentWhoseGrowthIsSpreadOverItsSubfolders) {
  const std::vector<FolderSize> before{folder("D:\\", 0_GiB)};
  const std::vector<FolderSize> now{folder("D:\\", 2_GiB), folder("D:\\a", 1_GiB),
                                    folder("D:\\b", 1_GiB)};
  const std::vector<FolderGrowth> grown = growth(before, now, 10);
  ASSERT_EQ(grown.size(), 3U);
  EXPECT_EQ(grown[0].path, fs::path{"D:\\"});
  EXPECT_EQ(grown[1].before, Bytes{});  // a folder new since (or below the threshold) counts from 0
}

TEST(Growth, KeepsTheLargestUpToTheLimit) {
  const std::vector<FolderSize> now{folder("E:\\x", 1_GiB), folder("E:\\y", 3_GiB),
                                    folder("E:\\z", 2_GiB)};
  const std::vector<FolderGrowth> grown = growth({}, now, 2);
  ASSERT_EQ(grown.size(), 2U);
  EXPECT_EQ(grown[0].path, fs::path{"E:\\y"});
  EXPECT_EQ(grown[1].path, fs::path{"E:\\z"});
}

TEST(IndexMigrations, AreAdditive) {
  const std::array sets{index_migrations()};
  EXPECT_TRUE(store::check_additive(sets));
}

class FolderHistoryTest : public ::testing::Test {
 public:
  FolderHistoryTest()
      : folder_(fs::temp_directory_path() /
                ("myclean-index-" +
                 std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    fs::remove_all(folder_);
    fs::create_directories(folder_);
    const std::array sets{index_migrations()};
    store_ = store::Store::open(folder_ / "myclean.db", clock_, sets).value();
  }
  FolderHistoryTest(const FolderHistoryTest&) = delete;
  FolderHistoryTest& operator=(const FolderHistoryTest&) = delete;
  FolderHistoryTest(FolderHistoryTest&&) = delete;
  FolderHistoryTest& operator=(FolderHistoryTest&&) = delete;
  ~FolderHistoryTest() override {
    store_.reset();
    std::error_code ignored;
    fs::remove_all(folder_, ignored);
  }

 protected:
  [[nodiscard]] WallTime now() const { return now_; }
  [[nodiscard]] WallTime days_ago(int days) const { return now_ - std::chrono::days{days}; }
  [[nodiscard]] store::Store& store() const { return *store_; }

  // The snapshot of C: at or before the time given; throws when there is none.
  static FolderSnapshot snapshot(FolderHistory& history, WallTime time) {
    std::optional<FolderSnapshot> found = history.snapshot_at("C:", time).value();
    if (!found.has_value()) {
      throw std::runtime_error("no snapshot");
    }
    return std::move(*found);
  }

 private:
  fs::path folder_;
  WallTime now_{std::chrono::sys_days{std::chrono::year{2026} / 10 / 31} + 12h};
  fakes::FakeClock clock_{now_};
  std::unique_ptr<store::Store> store_;
};

TEST_F(FolderHistoryTest, ReturnsTheNewestSnapshotAtOrBeforeATime) {
  FolderHistory history{store()};
  ASSERT_TRUE(history.save("C:", days_ago(8), std::vector{folder("C:\\Temp", 1_GiB)}));
  ASSERT_TRUE(history.save("C:", days_ago(2), std::vector{folder("C:\\Temp", 2_GiB)}));
  ASSERT_TRUE(history.save("D:", days_ago(1), std::vector{folder("D:\\x", 9_GiB)}));

  const FolderSnapshot week = snapshot(history, days_ago(7));
  EXPECT_EQ(week.time, days_ago(8));
  EXPECT_EQ(week.folders, std::vector{folder("C:\\Temp", 1_GiB)});
  EXPECT_EQ(snapshot(history, now()).folders.front().size_on_disk, 2_GiB);
  EXPECT_FALSE(history.snapshot_at("C:", days_ago(9)).value());
}

TEST_F(FolderHistoryTest, ReplacesASnapshotTakenAtTheSameSecond) {
  FolderHistory history{store()};
  ASSERT_TRUE(
      history.save("C:", now(), std::vector{folder("C:\\a", 1_GiB), folder("C:\\b", 1_GiB)}));
  ASSERT_TRUE(history.save("C:", now(), std::vector{folder("C:\\a", 3_GiB)}));
  EXPECT_EQ(snapshot(history, now()).folders, std::vector{folder("C:\\a", 3_GiB)});
}

TEST_F(FolderHistoryTest, ReportsGrowthOverADayAWeekAndAMonth) {
  FolderHistory history{store()};
  ASSERT_TRUE(history.save("C:", days_ago(31), std::vector{folder("C:\\Temp", 1_GiB)}));
  ASSERT_TRUE(history.save("C:", days_ago(7), std::vector{folder("C:\\Temp", 4_GiB)}));
  ASSERT_TRUE(history.save("C:", days_ago(1), std::vector{folder("C:\\Temp", 6_GiB)}));
  const std::vector current{folder("C:\\Temp", 7_GiB)};

  const auto grew = [&](std::chrono::seconds period) {
    const std::optional<GrowthReport> report =
        history.growth_over("C:", now(), period, current, 10).value();
    return report ? report->folders.front().before : Bytes{};
  };
  EXPECT_EQ(grew(24h), 6_GiB);
  EXPECT_EQ(grew(std::chrono::days{7}), 4_GiB);
  EXPECT_EQ(grew(std::chrono::days{30}), 1_GiB);
}

TEST_F(FolderHistoryTest, HasNoGrowthReportBeforeTheHistoryIsLongEnough) {
  FolderHistory history{store()};
  ASSERT_TRUE(history.save("C:", days_ago(3), std::vector{folder("C:\\Temp", 1_GiB)}));
  EXPECT_FALSE(history.growth_over("C:", now(), std::chrono::days{7}, {}, 10).value());
}

TEST_F(FolderHistoryTest, ForgetsSnapshotsOlderThan32Days) {
  FolderHistory history{store()};
  ASSERT_TRUE(history.add_retention());
  ASSERT_TRUE(history.save("C:", days_ago(33), std::vector{folder("C:\\Temp", 1_GiB)}));
  ASSERT_TRUE(history.save("C:", days_ago(31), std::vector{folder("C:\\Temp", 2_GiB)}));
  ASSERT_TRUE(store().run_retention());
  EXPECT_EQ(history.snapshot_at("C:", days_ago(32)).value(), std::nullopt);
  EXPECT_TRUE(history.snapshot_at("C:", days_ago(30)).value());
}

}  // namespace
}  // namespace myclean::index
