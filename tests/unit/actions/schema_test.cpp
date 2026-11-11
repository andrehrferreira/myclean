#include "core/actions/schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "actions_test_support.hpp"
#include "core/actions/audit_log.hpp"
#include "core/model/action.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/schema.hpp"

namespace myclean::actions {
namespace {

using testing::ActionsTest;

TEST(ActionMigrations, AreAdditive) {
  const std::vector<store::MigrationSet> sets = {store::store_migrations(), action_migrations()};
  const Result<void> checked = store::check_additive(sets);
  EXPECT_TRUE(checked.has_value()) << checked.error().message;
}

class ActionLogTest : public ActionsTest {
 protected:
  void append_one() {
    ASSERT_TRUE(store()
                    .write([&](store::Database& db) -> Result<void> {
                      Result<std::int64_t> row =
                          append_log(db, LogEntry{.time = testing::noon,
                                                  .plan = PlanId{7},
                                                  .actor = "owner",
                                                  .event = "transition",
                                                  .from = PlanState::awaiting_approval,
                                                  .to = PlanState::approved,
                                                  .outcome = "accepted"});
                      if (!row) {
                        return tl::unexpected(row.error());
                      }
                      return {};
                    })
                    .has_value());
  }

  // Runs `sql` in its own transaction; the error message, or empty on success.
  std::string run(const std::string& sql) {
    const Result<void> ran = store().write([&](store::Database& db) { return db.exec(sql); });
    return ran ? std::string{} : ran.error().message;
  }
};

TEST_F(ActionLogTest, EntryRoundTrips) {
  append_one();
  const std::vector<LogEntry> entries = log(PlanId{7});
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].time, testing::noon);
  EXPECT_EQ(entries[0].actor, "owner");
  EXPECT_EQ(entries[0].from, PlanState::awaiting_approval);
  EXPECT_EQ(entries[0].to, PlanState::approved);
  EXPECT_FALSE(entries[0].item.has_value());
}

// The Audit screen pages through the log newest first, by the ids of the rows.
TEST_F(ActionLogTest, PagesAreNewestFirstAndContinueBelowTheirLastId) {
  for (int entry = 0; entry < 5; ++entry) {
    append_one();
  }
  std::vector<LogRow> first;
  std::vector<LogRow> second;
  std::vector<LogRow> last;
  ASSERT_TRUE(store()
                  .read([&](store::Database& db) -> Result<void> {
                    first = log_page(db, std::nullopt, 2).value();
                    second = log_page(db, first.back().id, 2).value();
                    last = log_page(db, second.back().id, 10).value();
                    return {};
                  })
                  .has_value());
  ASSERT_EQ(first.size(), 2U);
  EXPECT_GT(first[0].id, first[1].id);
  EXPECT_EQ(first[0].entry.plan, PlanId{7});
  EXPECT_EQ(first[0].entry.to, PlanState::approved);
  ASSERT_EQ(second.size(), 2U);
  EXPECT_LT(second[0].id, first[1].id);
  ASSERT_EQ(last.size(), 1U);
}

TEST_F(ActionLogTest, UpdateRejectedByTrigger) {
  append_one();
  EXPECT_NE(run("UPDATE action_log SET actor = 'mcp:x'").find("append-only"), std::string::npos);
  EXPECT_EQ(log(PlanId{7}).at(0).actor, "owner");
}

TEST_F(ActionLogTest, DeleteRejectedByTrigger) {
  append_one();
  EXPECT_NE(run("DELETE FROM action_log").find("append-only"), std::string::npos);
  EXPECT_EQ(log(PlanId{7}).size(), 1U);
}

TEST_F(ActionLogTest, UndoDataRoundTripsAndIsMissingUntilStored) {
  append_one();
  const Undo undo{.items = {{.item = PlanItemId{3}, .data = R"(F:\q\1\3\a.tmp)"},
                            {.item = PlanItemId{4}, .data = std::string("\0\x01", 2)}},
                  .not_reversible = {}};
  Result<Undo> loaded = fail(Errc::io_error, "not read");
  ASSERT_TRUE(store()
                  .write([&](store::Database& db) -> Result<void> {
                    if (load_undo(db, PlanId{7}).error().code != Errc::not_found) {
                      return fail(Errc::invalid_state, "undo data before it was stored");
                    }
                    if (Result<void> saved = save_undo(db, 1, PlanId{7}, undo); !saved) {
                      return saved;
                    }
                    loaded = load_undo(db, PlanId{7});
                    return {};
                  })
                  .has_value());
  ASSERT_TRUE(loaded.has_value());
  ASSERT_EQ(loaded->items.size(), 2U);
  EXPECT_EQ(loaded->items[0].item, PlanItemId{3});
  EXPECT_EQ(loaded->items[0].data, undo.items[0].data);
  EXPECT_EQ(loaded->items[1].data, undo.items[1].data);
  EXPECT_TRUE(loaded->reversible());
}

TEST_F(ActionLogTest, PlanRetentionKeepsTheAuditLog) {
  const Plan plan = submit({}, AutomationLevel::suggest, Actor::scheduler());
  clock().advance(store::Store::history_retention + std::chrono::days{1});
  ASSERT_TRUE(store().run_retention().has_value());
  EXPECT_EQ(count("SELECT count(*) FROM plan"), 0);
  EXPECT_EQ(log(plan.id).size(), 2U);
}

}  // namespace
}  // namespace myclean::actions
