#include "core/store/migrations.hpp"

#include <array>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/actor.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"
#include "core/store/schema.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "store_test_support.hpp"

namespace myclean::store {
namespace {

using testing::count_rows;
using testing::noon;
using testing::ok;
using testing::TempDir;

const Migration create_event{
    .version = 1,
    .name = "event",
    .sql = "CREATE TABLE event(id INTEGER PRIMARY KEY, kind TEXT NOT NULL, size REAL);"};

MigrationSet module(std::vector<Migration> migrations) {
  return MigrationSet{.component = "test", .migrations = std::move(migrations)};
}

int version(Store& store, std::string_view component) {
  int found = -1;
  EXPECT_TRUE(ok(store.read([&](Database& db) -> Result<void> {
    Result<int> current = schema_version(db, component);
    if (!current) {
      return tl::unexpected(current.error());
    }
    found = *current;
    return {};
  })));
  return found;
}

Plan sample_plan() {
  return Plan{.action = "DeleteItems", .proposer = Actor::owner(), .created_at = noon};
}

TEST(Migrations, FreshDatabaseIsMigratedToTheLatestVersion) {
  const TempDir dir;
  const fakes::FakeClock clock(noon);
  const std::array modules{module(
      {create_event,
       {.version = 2, .name = "event_time", .sql = "ALTER TABLE event ADD COLUMN time INTEGER"}})};

  Result<std::unique_ptr<Store>> store = Store::open(dir.database(), clock, modules);
  ASSERT_TRUE(ok(store));

  EXPECT_EQ(version(**store, "store"), static_cast<int>(store_migrations().migrations.size()));
  EXPECT_EQ(version(**store, "test"), 2);
  EXPECT_TRUE(ok(testing::exec(**store, "INSERT INTO event(kind, time) VALUES('x', 1)")));
}

TEST(Migrations, FailedMigrationRollsBackAndKeepsTheLastGoodVersion) {
  const TempDir dir;
  const fakes::FakeClock clock(noon);
  const std::vector<Migration> good{
      create_event,
      {.version = 2, .name = "two", .sql = "CREATE TABLE two(a INTEGER);"},
      {.version = 3, .name = "three", .sql = "CREATE TABLE three(a INTEGER);"}};
  {
    const std::array modules{module(good)};
    ASSERT_TRUE(ok(Store::open(dir.database(), clock, modules)));
  }

  std::vector<Migration> with_broken = good;
  with_broken.push_back({.version = 4,
                         .name = "broken",
                         .sql = "CREATE TABLE half(a INTEGER); INSERT INTO missing VALUES(1);"});
  const std::array broken_modules{module(with_broken)};
  const Result<std::unique_ptr<Store>> failed = Store::open(dir.database(), clock, broken_modules);
  ASSERT_FALSE(failed.has_value());
  EXPECT_NE(failed.error().message.find("migration test 4 (broken) failed"), std::string::npos)
      << failed.error().message;

  const std::array modules{module(good)};
  Result<std::unique_ptr<Store>> store = Store::open(dir.database(), clock, modules);
  ASSERT_TRUE(ok(store));
  EXPECT_EQ(version(**store, "test"), 3);
  EXPECT_EQ(testing::query_int(
                **store, "SELECT count(*) FROM sqlite_schema WHERE name IN ('half', 'three')"),
            1);
}

// Build B2 migrates the store schema one version further; build B1 is rolled back to and opens
// that database.
TEST(Migrations, OlderBuildOpensNewerSchemaWithoutMigratingAndKeepsHistory) {
  const TempDir dir;
  const fakes::FakeClock clock(noon);
  PlanId old_plan;
  {
    Result<std::unique_ptr<Store>> b1 = Store::open(dir.database(), clock);
    ASSERT_TRUE(ok(b1));
    ASSERT_TRUE(ok((*b1)->write([&](Database& db) -> Result<void> {
      Result<Plan> stored = add_plan(db, sample_plan());
      if (!stored) {
        return tl::unexpected(stored.error());
      }
      old_plan = stored->id;
      return {};
    })));
  }
  MigrationSet b2 = store_migrations();
  const int b1_version = static_cast<int>(b2.migrations.size());
  b2.migrations.push_back({.version = b1_version + 1,
                           .name = "b2_additions",
                           .sql = "ALTER TABLE plan ADD COLUMN note TEXT NOT NULL DEFAULT '';"
                                  "CREATE TABLE b2_only(a INTEGER);"
                                  "INSERT INTO b2_only VALUES(1);"});
  {
    Result<Database> db = Database::open(dir.database());
    ASSERT_TRUE(ok(db));
    ASSERT_TRUE(ok(migrate(*db, b2)));
    ASSERT_TRUE(ok(db->exec("UPDATE plan SET note = 'from b2'")));
  }

  Result<std::unique_ptr<Store>> b1 = Store::open(dir.database(), clock);
  ASSERT_TRUE(ok(b1));
  EXPECT_EQ(version(**b1, "store"), b1_version + 1);
  ASSERT_TRUE(ok((*b1)->write([&](Database& db) -> Result<void> {
    Result<Plan> kept = get_plan(db, old_plan);
    if (!kept) {
      return tl::unexpected(kept.error());
    }
    EXPECT_EQ(kept->action, "DeleteItems");
    return add_plan(db, sample_plan()).map([](const Plan&) {});
  })));
  EXPECT_EQ(count_rows(**b1, "plan"), 2);
  EXPECT_EQ(count_rows(**b1, "b2_only"), 1);
}

TEST(Migrations, StoreMigrationsAreAdditive) {
  const std::array sets{store_migrations()};
  EXPECT_TRUE(ok(check_additive(sets)));
}

TEST(Migrations, AdditiveChangesAreAccepted) {
  const std::array sets{module({
      create_event,
      {.version = 2,
       .name = "additions",
       .sql = "ALTER TABLE event ADD COLUMN note TEXT;"
              "ALTER TABLE event ADD COLUMN level INTEGER NOT NULL DEFAULT 0;"
              "CREATE INDEX event_kind ON event(kind);"
              "CREATE TABLE other(a INTEGER);"
              "CREATE TRIGGER event_no_delete BEFORE DELETE ON event "
              "BEGIN SELECT RAISE(ABORT, 'append-only'); END;"},
  })};
  EXPECT_TRUE(ok(check_additive(sets)));
}

struct DestructiveCase {
  const char* name;
  const char* sql;

  // GoogleTest's printer hook, so test names show the case.
  friend void PrintTo(const DestructiveCase& change,  // NOLINT(readability-identifier-naming)
                      std::ostream* out) {
    *out << change.name;
  }
};

class DestructiveMigration : public ::testing::TestWithParam<DestructiveCase> {};

TEST_P(DestructiveMigration, IsRejectedNamingTheMigration) {
  const DestructiveCase& change = GetParam();
  const std::array sets{
      store_migrations(),
      module({create_event, {.version = 2, .name = change.name, .sql = change.sql}})};

  const Result<void> checked = check_additive(sets);

  ASSERT_FALSE(checked.has_value());
  EXPECT_EQ(checked.error().code, Errc::invalid_argument);
  EXPECT_NE(checked.error().message.find("migration test 2 (" + std::string(change.name) +
                                         ") is not additive"),
            std::string::npos)
      << checked.error().message;
}

INSTANTIATE_TEST_SUITE_P(
    Migrations, DestructiveMigration,
    ::testing::Values(
        DestructiveCase{"drop_column", "ALTER TABLE event DROP COLUMN size;"},
        DestructiveCase{"rename_column", "ALTER TABLE event RENAME COLUMN kind TO category;"},
        DestructiveCase{"drop_table", "DROP TABLE event;"},
        DestructiveCase{"rename_table", "ALTER TABLE event RENAME TO events;"},
        DestructiveCase{"drop_store_table", "DROP TABLE schedule_run;"},
        DestructiveCase{"change_type",
                        "CREATE TABLE event_new(id INTEGER PRIMARY KEY, kind TEXT NOT NULL, "
                        "size INTEGER);"
                        "INSERT INTO event_new SELECT * FROM event;"
                        "DROP TABLE event;"
                        "ALTER TABLE event_new RENAME TO event;"},
        DestructiveCase{"change_nullability",
                        "CREATE TABLE event_new(id INTEGER PRIMARY KEY, kind TEXT, size REAL);"
                        "DROP TABLE event;"
                        "ALTER TABLE event_new RENAME TO event;"}),
    [](const ::testing::TestParamInfo<DestructiveCase>& info) { return info.param.name; });

TEST(Migrations, OutOfOrderNumbersAreRejected) {
  const std::array sets{module({{.version = 2, .name = "skipped_one", .sql = "SELECT 1;"}})};
  const Result<void> checked = check_additive(sets);
  ASSERT_FALSE(checked.has_value());
  EXPECT_NE(checked.error().message.find("test 2 (skipped_one) is out of order"), std::string::npos)
      << checked.error().message;

  Result<Database> db = Database::open_in_memory();
  ASSERT_TRUE(ok(db));
  EXPECT_FALSE(migrate(*db, sets[0]).has_value());
}

}  // namespace
}  // namespace myclean::store
