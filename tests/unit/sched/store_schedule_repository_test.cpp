#include "core/sched/store_schedule_repository.hpp"

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
#include "core/sched/schedule_repository.hpp"
#include "core/store/database.hpp"
#include "core/store/schedule_repository.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::sched {
namespace {

using namespace std::chrono_literals;

const WallTime noon = std::chrono::sys_days{2026y / 10 / 31} + 12h;

// A fresh folder under %TEMP% for the current test, removed afterwards.
class TempFolder {
 public:
  TempFolder() {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;
  TempFolder(TempFolder&&) = delete;
  TempFolder& operator=(TempFolder&&) = delete;
  ~TempFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_ =
      std::filesystem::temp_directory_path() /
      ("myclean-sched-" +
       std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
};

class StoreScheduleRepositoryTest : public ::testing::Test {
 protected:
  [[nodiscard]] std::unique_ptr<store::Store> open() const {
    return store::Store::open(folder_.path() / "myclean.db", clock_).value();
  }

 private:
  TempFolder folder_;
  fakes::FakeClock clock_{noon};
};

ScheduleState report_job() {
  return {.name = "report",
          .expression = "0 9 1 * *",
          .last_run = noon - 24h,
          .last_result = RunStatus::succeeded,
          .next_run = noon + 24h};
}

TEST_F(StoreScheduleRepositoryTest, SavedStateSurvivesAReopen) {
  {
    const std::unique_ptr<store::Store> store = open();
    StoreScheduleRepository repository{*store};
    EXPECT_EQ(repository.load("report").value(), std::nullopt);
    ASSERT_TRUE(repository.save(report_job()).has_value());
  }  // no close(): a save is committed when it returns
  const std::unique_ptr<store::Store> store = open();
  StoreScheduleRepository repository{*store};
  EXPECT_EQ(repository.load("report").value(), report_job());
}

TEST_F(StoreScheduleRepositoryTest, SaveReplacesTheWholeRow) {
  const std::unique_ptr<store::Store> store = open();
  StoreScheduleRepository repository{*store};
  ASSERT_TRUE(repository.save(report_job()).has_value());
  const ScheduleState fresh{.name = "report", .expression = "0 * * * *", .next_run = noon + 1h};
  ASSERT_TRUE(repository.save(fresh).has_value());
  EXPECT_EQ(repository.load("report").value(), fresh);
}

TEST_F(StoreScheduleRepositoryTest, RunsAreAppendedInOrder) {
  const std::unique_ptr<store::Store> store = open();
  StoreScheduleRepository repository{*store};
  ASSERT_TRUE(repository
                  .append_run({.job = "report",
                               .started = noon,
                               .catch_up = true,
                               .status = RunStatus::failed,
                               .detail = "disk full"})
                  .has_value());
  ASSERT_TRUE(
      repository.append_run({.job = "report", .started = noon + 1h, .status = RunStatus::skipped})
          .has_value());
  std::vector<store::JobRun> runs;
  ASSERT_TRUE(store
                  ->read([&](store::Database& db) -> Result<void> {
                    Result<std::vector<store::JobRun>> read = store::job_runs(db, "report");
                    if (!read) {
                      return tl::unexpected(read.error());
                    }
                    runs = *std::move(read);
                    return {};
                  })
                  .has_value());
  ASSERT_EQ(runs.size(), 2U);
  EXPECT_EQ(runs[0].status, "failed");
  EXPECT_TRUE(runs[0].catch_up);
  EXPECT_EQ(runs[0].detail, "disk full");
  EXPECT_EQ(runs[1].status, "skipped");
  EXPECT_EQ(runs[1].started, noon + 1h);
}

TEST_F(StoreScheduleRepositoryTest, UnknownStoredResultIsInvalidData) {
  const std::unique_ptr<store::Store> store = open();
  ASSERT_TRUE(store
                  ->write([](store::Database& db) {
                    return store::save_job(db, {.name = "report",
                                                .expression = "0 9 1 * *",
                                                .last_result = "exploded",
                                                .next_run = noon});
                  })
                  .has_value());
  StoreScheduleRepository repository{*store};
  EXPECT_EQ(repository.load("report").error().code, Errc::invalid_data);
}

}  // namespace
}  // namespace myclean::sched
