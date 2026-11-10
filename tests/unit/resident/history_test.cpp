#include "core/resident/history.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/resident/sampler.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "resident_fixtures.hpp"

namespace myclean::resident {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::noon;

MinuteUse running(const std::string& key, Bytes private_bytes, Bytes ram = 0_B) {
  return MinuteUse{.key = key, .private_bytes = private_bytes, .ram = ram};
}

// The rows of resident_fine of `key`: (time, private bytes, samples).
std::vector<std::array<std::int64_t, 3>> fine_rows(store::Store& store, const std::string& key) {
  std::vector<std::array<std::int64_t, 3>> rows;
  EXPECT_TRUE(store.flush().has_value());
  EXPECT_TRUE(store
                  .read([&](store::Database& db) -> Result<void> {
                    Result<store::Statement> statement = db.prepare(
                        "SELECT time, private_bytes, samples FROM resident_fine WHERE key = ?1 "
                        "ORDER BY time");
                    if (!statement) {
                      return tl::unexpected(statement.error());
                    }
                    statement->bind(1, std::string_view{key});
                    while (statement->step().value_or(false)) {
                      rows.push_back({statement->column_int(0), statement->column_int(1),
                                      statement->column_int(2)});
                    }
                    return {};
                  })
                  .has_value());
  return rows;
}

TEST(ResidentSchemaTest, MigrationsAreAdditiveAndRetentionKeepsTheSpans) {
  const std::array sets{resident_migrations()};
  EXPECT_TRUE(store::check_additive(sets));
  const std::vector<store::RetentionRule> rules = resident_retention();
  ASSERT_EQ(rules.size(), 2U);
  EXPECT_EQ(rules[0].table, "resident_fine");
  EXPECT_EQ(rules[0].max_age, std::chrono::seconds{fine_span});
  EXPECT_EQ(rules[1].table, "resident_hour");
  EXPECT_EQ(rules[1].max_age, std::chrono::seconds{hour_span});
}

class HistoryTest : public testing::StoreTest {};

TEST_F(HistoryTest, AveragesEachFiveMinuteBucketOnceItIsOver) {
  History history(store());
  for (int minute = 0; minute < 5; ++minute) {
    const std::vector<MinuteUse> use = {
        running("app.exe", Bytes{(100ULL + (10ULL * static_cast<unsigned>(minute))) << 20U})};
    EXPECT_FALSE(history.add(noon + std::chrono::minutes{minute}, use));
  }
  EXPECT_TRUE(history.trends().empty());
  const std::vector<MinuteUse> next = {running("app.exe", 200_MiB)};
  EXPECT_TRUE(history.add(noon + 5min, next));
  ASSERT_EQ(history.trends().size(), 1U);
  const std::vector<Point>& trend = history.trends().at("app.exe");
  ASSERT_EQ(trend.size(), 1U);
  EXPECT_EQ(trend[0], (Point{.time = noon, .private_bytes = 120_MiB, .samples = 5}));
  const auto rows = fine_rows(store(), "app.exe");
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0][0], store::to_unix_seconds(noon));
  EXPECT_EQ(rows[0][1], static_cast<std::int64_t>((120_MiB).count()));
  EXPECT_EQ(rows[0][2], 5);
}

TEST_F(HistoryTest, HoursAddUpToTheCostOfEachResidentProgram) {
  History history(store());
  for (int minute = 0; minute < 60; ++minute) {
    std::vector<MinuteUse> use = {MinuteUse{.key = "armourycrate.service.exe",
                                            .private_bytes = 300_MiB,
                                            .ram = Bytes{(minute == 30 ? 400ULL : 100ULL) << 20U},
                                            .cpu_seconds = 0.5,
                                            .disk_bytes = 1000,
                                            .net_bytes = 10}};
    if (minute < 10) {
      use.push_back(MinuteUse{.key = "setup.exe", .ram = 50_MiB, .cpu_seconds = 3.0});
    }
    // Network bytes of a program that runs no more count, but not as time running.
    if (minute == 59) {
      use.push_back(MinuteUse{.key = "updater.exe", .running = false, .net_bytes = 500});
    }
    static_cast<void>(history.add(noon + std::chrono::minutes{minute}, use));
  }
  static_cast<void>(history.add(noon + 60min, std::vector<MinuteUse>{}));
  ASSERT_TRUE(store().flush().has_value());

  const Result<std::vector<Cost>> all = costs(store(), noon, 1min);
  ASSERT_TRUE(all.has_value()) << all.error().message;
  ASSERT_EQ(all->size(), 2U);
  // Most CPU first.
  EXPECT_EQ(all->at(0), (Cost{.key = "armourycrate.service.exe",
                              .running = 60min,
                              .cpu_seconds = 30.0,
                              .ram_average = Bytes{(100ULL << 20U) + ((300ULL << 20U) / 60)},
                              .ram_peak = 400_MiB,
                              .disk_bytes = 60'000,
                              .net_bytes = 600}));
  EXPECT_EQ(all->at(1).key, "setup.exe");
  EXPECT_EQ(all->at(1).running, 10min);
  // Only programs that ran long enough are resident.
  const Result<std::vector<Cost>> resident = costs(store(), noon, 30min);
  ASSERT_TRUE(resident.has_value());
  ASSERT_EQ(resident->size(), 1U);
  EXPECT_EQ(resident->front().key, "armourycrate.service.exe");
  // Hours before `from` are left out.
  EXPECT_TRUE(costs(store(), noon + 1h, 1min).value().empty());
  // One program over a span: the same totals; nothing outside the span or for another key.
  EXPECT_EQ(cost_of(store(), "armourycrate.service.exe", noon, noon + 1h).value(), all->at(0));
  EXPECT_EQ(cost_of(store(), "armourycrate.service.exe", noon + 1h, noon + 2h).value(),
            Cost{.key = "armourycrate.service.exe"});
  EXPECT_EQ(cost_of(store(), "searchindexer.exe", noon, noon + 1h)->running, 0min);
}

TEST_F(HistoryTest, ABucketWrittenAtAStopIsAddedToAfterTheRestart) {
  {
    History history(store());
    static_cast<void>(history.add(noon, std::vector<MinuteUse>{running("app.exe", 100_MiB)}));
    static_cast<void>(
        history.add(noon + 1min, std::vector<MinuteUse>{running("app.exe", 100_MiB)}));
    history.flush();
  }
  reopen();
  History history(store());
  ASSERT_TRUE(history.load(noon + 2min).has_value());
  ASSERT_EQ(history.trends().at("app.exe"),
            (std::vector<Point>{{.time = noon, .private_bytes = 100_MiB, .samples = 2}}));
  for (int minute = 2; minute < 5; ++minute) {
    static_cast<void>(history.add(noon + std::chrono::minutes{minute},
                                  std::vector<MinuteUse>{running("app.exe", 200_MiB)}));
  }
  EXPECT_TRUE(history.add(noon + 5min, std::vector<MinuteUse>{running("app.exe", 200_MiB)}));
  EXPECT_EQ(history.trends().at("app.exe"),
            (std::vector<Point>{{.time = noon, .private_bytes = 160_MiB, .samples = 5}}));
  const auto rows = fine_rows(store(), "app.exe");
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0][1], static_cast<std::int64_t>((160_MiB).count()));
  EXPECT_EQ(rows[0][2], 5);
}

TEST_F(HistoryTest, KeepsTheBucketsOfTheLastTwoDays) {
  History history(store());
  static_cast<void>(history.add(noon, std::vector<MinuteUse>{running("old.exe", 10_MiB)}));
  static_cast<void>(history.add(noon + 5min, std::vector<MinuteUse>{running("new.exe", 10_MiB)}));
  EXPECT_EQ(history.trends().size(), 1U);
  static_cast<void>(
      history.add(noon + fine_span + 10min, std::vector<MinuteUse>{running("new.exe", 10_MiB)}));
  // Both buckets, at 12:00 and 12:05, are now more than two days old.
  EXPECT_TRUE(history.trends().empty());

  // load() reads only the buckets of the last two days.
  ASSERT_TRUE(store().flush().has_value());
  History loaded(store());
  ASSERT_TRUE(loaded.load(noon + fine_span + 3min).has_value());
  EXPECT_FALSE(loaded.trends().contains("old.exe"));
  EXPECT_TRUE(loaded.trends().contains("new.exe"));
}

}  // namespace
}  // namespace myclean::resident
