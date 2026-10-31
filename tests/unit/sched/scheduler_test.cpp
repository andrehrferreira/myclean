#include "core/sched/scheduler.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/sched/schedule_repository.hpp"
#include "fakes/fake_clock.hpp"

namespace myclean::sched {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view zone_name = "Europe/Berlin";

WallTime local(std::chrono::year_month_day date, std::chrono::minutes time_of_day) {
  return std::chrono::locate_zone(zone_name)->to_sys(std::chrono::local_days{date} + time_of_day,
                                                     std::chrono::choose::earliest);
}

// The monthly report job of the spec scenarios, last run 1 October 09:00.
ScheduleState monthly_job() {
  return {.name = "report",
          .expression = "0 9 1 * *",
          .last_run = local(2026y / 10 / 1, 9h),
          .last_result = RunStatus::succeeded,
          .next_run = local(2026y / 11 / 1, 9h)};
}

// Lets time pass minute by minute, polling each minute and finishing every run at once.
// Returns the runs started.
std::vector<WallTime> run_for(fakes::FakeClock& clock, Scheduler& scheduler,
                              std::chrono::minutes span) {
  std::vector<WallTime> started;
  for (auto passed = 0min; passed < span; passed += 1min) {
    clock.advance(1min);
    for (const DueRun& run : scheduler.poll().runs) {
      started.push_back(clock.now());
      EXPECT_TRUE(scheduler.finish(run.job, {}).has_value());
    }
  }
  return started;
}

TEST(Scheduler, InvalidExpressionRegistersNothing) {
  const fakes::FakeClock clock{local(2026y / 10 / 31, 12h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};

  const auto added = scheduler.add("report", "0 25 * * *");
  ASSERT_FALSE(added.has_value());
  EXPECT_TRUE(added.error().message.starts_with("hour field"));
  EXPECT_TRUE(repository.rows().empty());
  EXPECT_EQ(scheduler.next_wake(), std::nullopt);
  EXPECT_TRUE(scheduler.poll().runs.empty());
}

TEST(Scheduler, NeverMatchingExpressionRegistersNothing) {
  const fakes::FakeClock clock{local(2026y / 10 / 31, 12h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};

  EXPECT_EQ(scheduler.add("report", "0 0 30 2 *").error().code, Errc::invalid_argument);
  EXPECT_TRUE(repository.rows().empty());
}

TEST(Scheduler, DuplicateNameRejected) {
  const fakes::FakeClock clock{local(2026y / 10 / 31, 12h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};

  ASSERT_TRUE(scheduler.add("report", "0 9 1 * *").has_value());
  EXPECT_EQ(scheduler.add("report", "0 9 * * *").error().code, Errc::already_exists);
  EXPECT_EQ(repository.rows().at("report").expression, "0 9 1 * *");
}

TEST(Scheduler, NewJobIsPersistedWithItsNextRun) {
  const fakes::FakeClock clock{local(2026y / 10 / 31, 12h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};

  ASSERT_TRUE(scheduler.add("report", "0 9 1 * *").has_value());
  EXPECT_EQ(repository.rows().at("report"), (ScheduleState{.name = "report",
                                                           .expression = "0 9 1 * *",
                                                           .next_run = local(2026y / 11 / 1, 9h)}));
  EXPECT_EQ(scheduler.next_wake(), local(2026y / 11 / 1, 9h));
}

TEST(Scheduler, StateSurvivesRestart) {
  const fakes::FakeClock clock{local(2026y / 10 / 15, 12h), zone_name};
  MemoryScheduleRepository repository;
  ASSERT_TRUE(repository.save(monthly_job()).has_value());
  Scheduler scheduler{clock, repository};

  ASSERT_TRUE(scheduler.add("report", "0 9 1 * *").has_value());
  EXPECT_EQ(repository.rows().at("report"), monthly_job());
  EXPECT_EQ(scheduler.next_wake(), local(2026y / 11 / 1, 9h));
  EXPECT_TRUE(scheduler.poll().runs.empty());
}

TEST(Scheduler, ChangedExpressionRecomputesNextRun) {
  const fakes::FakeClock clock{local(2026y / 10 / 15, 12h), zone_name};
  MemoryScheduleRepository repository;
  ASSERT_TRUE(repository.save(monthly_job()).has_value());
  Scheduler scheduler{clock, repository};

  ASSERT_TRUE(scheduler.add("report", "0 9 * * *").has_value());
  const ScheduleState& row = repository.rows().at("report");
  EXPECT_EQ(row.expression, "0 9 * * *");
  EXPECT_EQ(row.last_run, monthly_job().last_run);
  EXPECT_EQ(row.next_run, local(2026y / 10 / 16, 9h));
}

// Also the "catch-up tested with a fake clock" scenario: no real-time wait anywhere.
TEST(Scheduler, MonthlyJobMissedWhileOffRunsOnceAsCatchUp) {
  const fakes::FakeClock clock{local(2026y / 11 / 3, 8h), zone_name};
  MemoryScheduleRepository repository;
  ASSERT_TRUE(repository.save(monthly_job()).has_value());
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("report", "0 9 1 * *").has_value());

  const PollResult poll = scheduler.poll();
  EXPECT_TRUE(poll.errors.empty());
  EXPECT_EQ(poll.runs, (std::vector<DueRun>{{.job = "report", .catch_up = true}}));
  ASSERT_TRUE(scheduler.finish("report", {}).has_value());

  EXPECT_EQ(repository.runs(), (std::vector<RunRecord>{{.job = "report",
                                                        .started = local(2026y / 11 / 3, 8h),
                                                        .catch_up = true,
                                                        .status = RunStatus::succeeded}}));
  EXPECT_EQ(repository.rows().at("report"), (ScheduleState{.name = "report",
                                                           .expression = "0 9 1 * *",
                                                           .last_run = local(2026y / 11 / 3, 8h),
                                                           .last_result = RunStatus::succeeded,
                                                           .next_run = local(2026y / 12 / 1, 9h)}));
  EXPECT_TRUE(scheduler.poll().runs.empty());
}

TEST(Scheduler, SeveralMissedOccurrencesCoalesced) {
  fakes::FakeClock clock{local(2026y / 10 / 31, 10h + 30min), zone_name};
  MemoryScheduleRepository repository;
  {
    Scheduler before{clock, repository};
    ASSERT_TRUE(before.add("hourly", "0 * * * *").has_value());
  }
  clock.advance(5h);  // MyClean is off from 10:30 to 15:30
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("hourly", "0 * * * *").has_value());

  EXPECT_EQ(scheduler.poll().runs, (std::vector<DueRun>{{.job = "hourly", .catch_up = true}}));
  ASSERT_TRUE(scheduler.finish("hourly", {}).has_value());
  EXPECT_TRUE(scheduler.poll().runs.empty());
  EXPECT_EQ(repository.runs().size(), 1U);
  EXPECT_EQ(scheduler.next_wake(), local(2026y / 10 / 31, 16h));
}

TEST(Scheduler, NothingMissedWaitsForTheNextRun) {
  fakes::FakeClock clock{local(2026y / 10 / 31, 10h + 30min), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("hourly", "0 * * * *").has_value());

  EXPECT_TRUE(scheduler.poll().runs.empty());
  clock.advance(29min);
  EXPECT_TRUE(scheduler.poll().runs.empty());
  clock.advance(1min);
  EXPECT_EQ(scheduler.poll().runs, (std::vector<DueRun>{{.job = "hourly", .catch_up = false}}));
}

TEST(Scheduler, RepeatedHourAtEndOfDaylightSavingRunsOnce) {
  fakes::FakeClock clock{local(2026y / 10 / 25, 0h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("nightly", "30 2 * * *").has_value());

  // Five real hours: 00:00 to 04:00 local, with 02:00-03:00 passing twice.
  EXPECT_EQ(run_for(clock, scheduler, 5h),
            std::vector<WallTime>{std::chrono::sys_days{2026y / 10 / 25} + 30min});
}

TEST(Scheduler, HourSkippedByDaylightSavingStillRunsOnce) {
  fakes::FakeClock clock{local(2026y / 3 / 29, 0h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("nightly", "30 2 * * *").has_value());

  EXPECT_EQ(run_for(clock, scheduler, 4h),
            std::vector<WallTime>{std::chrono::sys_days{2026y / 3 / 29} + 1h});
}

TEST(Scheduler, ClockMovedBackDoesNotRepeatAnOccurrence) {
  fakes::FakeClock clock{local(2026y / 10 / 31, 8h + 59min), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("daily", "0 9 * * *").has_value());

  EXPECT_EQ(run_for(clock, scheduler, 1min), std::vector<WallTime>{local(2026y / 10 / 31, 9h)});
  clock.set_wall(local(2026y / 10 / 31, 8h + 30min));  // the owner sets the clock back
  EXPECT_TRUE(run_for(clock, scheduler, 2h).empty());
  EXPECT_EQ(scheduler.next_wake(), local(2026y / 11 / 1, 9h));
}

TEST(Scheduler, LongRunBlocksTheNextOccurrence) {
  fakes::FakeClock clock{local(2026y / 10 / 31, 10h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("scan", "*/15 * * * *").has_value());

  clock.advance(15min);
  ASSERT_EQ(scheduler.poll().runs.size(), 1U);  // 10:15
  clock.advance(15min);
  EXPECT_TRUE(scheduler.poll().runs.empty());  // 10:30, the 10:15 run is still active
  ASSERT_EQ(repository.runs().size(), 1U);
  EXPECT_EQ(repository.runs().at(0),
            (RunRecord{.job = "scan",
                       .started = local(2026y / 10 / 31, 10h + 30min),
                       .status = RunStatus::skipped,
                       .detail = "skipped: the previous run is still active"}));
  EXPECT_EQ(repository.rows().at("scan").last_result, RunStatus::running);

  clock.advance(5min);
  ASSERT_TRUE(scheduler.finish("scan", {}).has_value());
  EXPECT_EQ(repository.runs().at(1).status, RunStatus::succeeded);
  EXPECT_EQ(repository.runs().at(1).started, local(2026y / 10 / 31, 10h + 15min));
  clock.advance(10min);
  EXPECT_EQ(scheduler.poll().runs.size(), 1U);  // 10:45
}

TEST(Scheduler, FailedRunIsRecordedWithItsError) {
  fakes::FakeClock clock{local(2026y / 10 / 31, 10h), zone_name};
  MemoryScheduleRepository repository;
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("scan", "*/15 * * * *").has_value());
  clock.advance(15min);
  ASSERT_EQ(scheduler.poll().runs.size(), 1U);

  ASSERT_TRUE(scheduler.finish("scan", fail(Errc::io_error, "disk full")).has_value());
  EXPECT_EQ(repository.runs().at(0).status, RunStatus::failed);
  EXPECT_EQ(repository.runs().at(0).detail, "disk full");
  EXPECT_EQ(repository.rows().at("scan").last_result, RunStatus::failed);

  EXPECT_EQ(scheduler.finish("scan", {}).error().code, Errc::invalid_state);
  EXPECT_EQ(scheduler.finish("other", {}).error().code, Errc::not_found);
}

TEST(Scheduler, RunInterruptedByStopIsRecordedAsFailedAndNotRepeated) {
  fakes::FakeClock clock{local(2026y / 10 / 31, 10h), zone_name};
  MemoryScheduleRepository repository;
  {
    Scheduler before{clock, repository};
    ASSERT_TRUE(before.add("scan", "*/15 * * * *").has_value());
    clock.advance(15min);
    ASSERT_EQ(before.poll().runs.size(), 1U);  // MyClean stops while this run is active
  }
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("scan", "*/15 * * * *").has_value());

  EXPECT_EQ(repository.rows().at("scan").last_result, RunStatus::failed);
  ASSERT_EQ(repository.runs().size(), 1U);
  EXPECT_EQ(repository.runs().at(0).status, RunStatus::failed);
  EXPECT_TRUE(repository.runs().at(0).detail.starts_with("interrupted"));
  EXPECT_TRUE(scheduler.poll().runs.empty());
}

// Saves fail while `broken` is set.
class BrokenRepository final : public IScheduleRepository {
 public:
  Result<std::optional<ScheduleState>> load(std::string_view name) override {
    return inner.load(name);
  }
  Result<void> save(const ScheduleState& state) override {
    return broken ? fail(Errc::io_error, "disk I/O error") : inner.save(state);
  }
  Result<void> append_run(const RunRecord& record) override { return inner.append_run(record); }

  MemoryScheduleRepository inner;
  bool broken = false;
};

TEST(Scheduler, RunDoesNotStartWhenItsNextRunCannotBeSaved) {
  fakes::FakeClock clock{local(2026y / 10 / 31, 10h), zone_name};
  BrokenRepository repository;
  Scheduler scheduler{clock, repository};
  ASSERT_TRUE(scheduler.add("scan", "*/15 * * * *").has_value());
  clock.advance(15min);

  repository.broken = true;
  const PollResult failed = scheduler.poll();
  EXPECT_TRUE(failed.runs.empty());
  ASSERT_EQ(failed.errors.size(), 1U);
  EXPECT_EQ(failed.errors.at(0).code, Errc::io_error);

  repository.broken = false;
  EXPECT_EQ(scheduler.poll().runs.size(), 1U);
}

}  // namespace
}  // namespace myclean::sched
