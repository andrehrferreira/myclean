#include "core/store/store.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/actor.hpp"
#include "core/model/finding.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/plan_repository.hpp"
#include "fakes/fake_clock.hpp"
#include "store_test_support.hpp"

namespace myclean::store {
namespace {

using namespace std::chrono_literals;
using testing::count_rows;
using testing::noon;
using testing::ok;
using testing::TempDir;

// Stand-ins for tables other modules own: a buffered event table and the audit log.
const std::array<MigrationSet, 2> modules{
    MigrationSet{.component = "events_test",
                 .migrations = {{.version = 1,
                                 .name = "event",
                                 .sql = "CREATE TABLE event(id INTEGER PRIMARY KEY, "
                                        "time INTEGER NOT NULL, what TEXT NOT NULL);"}}},
    MigrationSet{.component = "audit_test",
                 .migrations = {{.version = 1,
                                 .name = "action_log",
                                 .sql = "CREATE TABLE action_log(id INTEGER PRIMARY KEY, "
                                        "time INTEGER NOT NULL, transition TEXT NOT NULL);"}}}};

void enqueue_event(Store& store, WallTime time, const std::string& what) {
  store.enqueue("INSERT INTO event(time, what) VALUES(?1, ?2)", {to_unix_seconds(time), what});
}

class StoreTest : public ::testing::Test {
 protected:
  void SetUp() override { reopen(); }

  void reopen() {
    store_.reset();
    Result<std::unique_ptr<Store>> opened = Store::open(dir().database(), clock(), modules);
    ASSERT_TRUE(ok(opened));
    store_ = std::move(*opened);
  }

  Store& store() { return *store_; }
  [[nodiscard]] const TempDir& dir() const { return dir_; }
  fakes::FakeClock& clock() { return clock_; }

 private:
  TempDir dir_;
  fakes::FakeClock clock_{noon + 30s};
  std::unique_ptr<Store> store_;
};

TEST_F(StoreTest, BufferedWritesAreCommittedTogetherTenSecondsAfterThePreviousFlush) {
  ASSERT_TRUE(ok(store().tick()));  // the first tick flushes, starting the 10 s interval

  for (int index = 0; index < 500; ++index) {
    enqueue_event(store(), clock().now(), "event " + std::to_string(index));
    clock().advance(8ms);
    ASSERT_TRUE(ok(store().tick()));
  }
  EXPECT_EQ(count_rows(store(), "event"), 0);

  clock().advance(9s - 4s);  // 9 s since the previous flush
  ASSERT_TRUE(ok(store().tick()));
  EXPECT_EQ(count_rows(store(), "event"), 0);

  clock().advance(1s);
  ASSERT_TRUE(ok(store().tick()));
  EXPECT_EQ(count_rows(store(), "event"), 500);
}

TEST_F(StoreTest, FlushIsOneTransaction) {
  for (int index = 0; index < 499; ++index) {
    enqueue_event(store(), clock().now(), "fine");
  }
  store().enqueue("INSERT INTO event(time, what) VALUES(?1, ?2)", {to_unix_seconds(noon), nullptr});

  const Result<void> flushed = store().flush();

  ASSERT_FALSE(flushed.has_value());
  EXPECT_NE(flushed.error().message.find("500 buffered writes were lost"), std::string::npos)
      << flushed.error().message;
  EXPECT_EQ(count_rows(store(), "event"), 0);
}

TEST_F(StoreTest, CleanShutdownFlushesTheBuffer) {
  for (int index = 0; index < 20; ++index) {
    enqueue_event(store(), clock().now(), "closing");
  }
  ASSERT_TRUE(ok(store().close()));
  reopen();
  EXPECT_EQ(count_rows(store(), "event"), 20);

  for (int index = 0; index < 20; ++index) {
    enqueue_event(store(), clock().now(), "destroyed");
  }
  reopen();  // destroys the store without an explicit close()
  EXPECT_EQ(count_rows(store(), "event"), 40);
}

TEST_F(StoreTest, AuditWriteSurvivesACrashWhileBufferedTelemetryMayNot) {
  enqueue_event(store(), clock().now(), "buffered");
  ASSERT_TRUE(ok(store().write([&](Database& db) -> Result<void> {
    Result<Statement> insert =
        db.prepare("INSERT INTO action_log(time, transition) VALUES(?1, 'applying')");
    if (!insert) {
      return tl::unexpected(insert.error());
    }
    return insert->bind(1, to_unix_seconds(clock().now())).run();
  })));

  // The process dies now: the files on disk are all that is left.
  const std::filesystem::path crashed = dir().path() / "crashed.db";
  std::filesystem::copy_file(dir().database(), crashed);
  std::filesystem::copy_file(dir().database().string() + "-wal", crashed.string() + "-wal");

  Result<std::unique_ptr<Store>> restarted = Store::open(crashed, clock(), modules);
  ASSERT_TRUE(ok(restarted));
  EXPECT_EQ(testing::query_int(**restarted,
                               "SELECT count(*) FROM action_log WHERE transition = 'applying'"),
            1);
  EXPECT_EQ(count_rows(**restarted, "event"), 0);
}

TEST_F(StoreTest, RetentionDeletesMinuteRowsOlderThan30Days) {
  ASSERT_TRUE(ok(testing::exec(store(),
                               "INSERT INTO series(id, metric, entity) VALUES(1, 'cpu.core', '3');"
                               "INSERT INTO ts_minute(series, time, min, max, mean, count) VALUES"
                               "(1, " +
                                   std::to_string(to_unix_seconds(clock().now() - 24h * 31)) +
                                   ", 1, 1, 1, 60),"
                                   "(1, " +
                                   std::to_string(to_unix_seconds(clock().now() - 24h * 29)) +
                                   ", 2, 2, 2, 60);")));

  ASSERT_TRUE(ok(store().run_retention()));

  const Result<std::vector<Aggregate>> rows =
      store().minutes({.metric = "cpu.core", .entity = "3"}, noon - 24h * 40, noon);
  ASSERT_TRUE(ok(rows));
  ASSERT_EQ(rows->size(), 1U);
  EXPECT_EQ(rows->front().mean, 2.0);
}

TEST_F(StoreTest, RetentionDeletesHoursFindingsAndPlansOlderThanTwoYears) {
  const WallTime old_time = clock().now() - 24h * 731;
  const WallTime recent_time = clock().now() - 24h * 729;
  ASSERT_TRUE(
      ok(testing::exec(store(),
                       "INSERT INTO series(id, metric, entity) VALUES(1, 'mem.available', '');"
                       "INSERT INTO ts_hour(series, time, min, max, mean, count) VALUES"
                       "(1, " +
                           std::to_string(to_unix_seconds(old_time)) +
                           ", 1, 1, 1, 3600),"
                           "(1, " +
                           std::to_string(to_unix_seconds(recent_time)) + ", 2, 2, 2, 3600);")));
  ASSERT_TRUE(ok(store().write([&](Database& db) -> Result<void> {
    for (const WallTime time : {old_time, recent_time}) {
      Result<FindingId> finding = add_finding(
          db, Finding{.kind = "clean.cache", .subject = "C:", .summary = "s", .detected_at = time});
      if (!finding) {
        return tl::unexpected(finding.error());
      }
      Result<Plan> plan = add_plan(db, Plan{.action = "DeleteItems",
                                            .finding = *finding,
                                            .proposer = Actor::scheduler(),
                                            .created_at = time,
                                            .items = {PlanItem{.path = "C:\\cache\\a.tmp"}}});
      if (!plan) {
        return tl::unexpected(plan.error());
      }
    }
    return {};
  })));

  ASSERT_TRUE(ok(store().run_retention()));

  EXPECT_EQ(count_rows(store(), "ts_hour"), 1);
  EXPECT_EQ(count_rows(store(), "finding"), 1);
  EXPECT_EQ(count_rows(store(), "plan"), 1);
  EXPECT_EQ(count_rows(store(), "plan_item"), 1);
  EXPECT_EQ(testing::query_int(store(), "SELECT MIN(created_at) FROM plan"),
            to_unix_seconds(recent_time));
}

TEST_F(StoreTest, RetentionNeverTouchesTheAuditLog) {
  ASSERT_TRUE(
      ok(testing::exec(store(), "INSERT INTO action_log(time, transition) VALUES(" +
                                    std::to_string(to_unix_seconds(clock().now() - 24h * 365 * 3)) +
                                    ", 'applied')")));

  for (const char* table : {"action_log", "undo_blob", "schedule", "snapshot", "report"}) {
    const Result<void> added = store().add_retention(
        {.table = table, .time_column = "time", .max_age = Store::history_retention});
    ASSERT_FALSE(added.has_value()) << table;
    EXPECT_EQ(added.error().code, Errc::refused);
  }
  ASSERT_TRUE(ok(store().run_retention()));

  EXPECT_EQ(count_rows(store(), "action_log"), 1);
}

TEST_F(StoreTest, ModuleRetentionRuleRunsFromTickEveryHour) {
  ASSERT_TRUE(ok(store().add_retention(
      {.table = "event", .time_column = "time", .max_age = Store::history_retention})));
  EXPECT_FALSE(store()
                   .add_retention({.table = "event; DROP TABLE plan", .time_column = "time"})
                   .has_value());
  const auto insert_event_aged = [&](std::chrono::hours age) {
    ASSERT_TRUE(ok(testing::exec(store(), "INSERT INTO event(time, what) VALUES(" +
                                              std::to_string(to_unix_seconds(clock().now() - age)) +
                                              ", 'e')")));
  };
  insert_event_aged(24h * 729);
  insert_event_aged(24h * 731);

  ASSERT_TRUE(ok(store().tick()));
  EXPECT_EQ(count_rows(store(), "event"), 1);

  insert_event_aged(24h * 731);
  clock().advance(59min);
  ASSERT_TRUE(ok(store().tick()));
  EXPECT_EQ(count_rows(store(), "event"), 2);

  clock().advance(1min);
  ASSERT_TRUE(ok(store().tick()));
  EXPECT_EQ(count_rows(store(), "event"), 1);
}

}  // namespace
}  // namespace myclean::store
