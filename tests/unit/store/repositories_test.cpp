#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/actor.hpp"
#include "core/model/file_identity.hpp"
#include "core/model/finding.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/store/database.hpp"
#include "core/store/plan_repository.hpp"
#include "core/store/schedule_repository.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "store_test_support.hpp"

namespace myclean::store {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;
using testing::count_rows;
using testing::noon;
using testing::ok;
using testing::TempDir;

class RepositoryTest : public ::testing::Test {
 protected:
  void SetUp() override { reopen(); }

  void reopen() {
    store_.reset();
    Result<std::unique_ptr<Store>> opened = Store::open(dir_.database(), clock_);
    ASSERT_TRUE(ok(opened));
    store_ = std::move(*opened);
  }

  // Runs `work` inside Store::write and returns what it produced.
  template <typename T>
  Result<T> in_write(const std::function<Result<T>(Database&)>& work) {
    std::optional<Result<T>> produced;
    const Result<void> written = store().write([&](Database& db) -> Result<void> {
      produced = work(db);
      if (!*produced) {
        return tl::unexpected(produced->error());
      }
      return {};
    });
    if (!written) {
      return tl::unexpected(written.error());
    }
    return std::move(*produced);
  }

  Store& store() { return *store_; }

 private:
  TempDir dir_;
  fakes::FakeClock clock_{noon};
  std::unique_ptr<Store> store_;
};

FileIdentity identity(std::uint8_t seed) {
  FileIdentity id{.volume_serial = 0xFEDC'BA98'7654'3210ULL};
  for (std::size_t index = 0; index < id.file_id.size(); ++index) {
    id.file_id.at(index) = static_cast<std::uint8_t>(seed + index);
  }
  return id;
}

Plan cleanup_plan(std::optional<FindingId> finding) {
  return Plan{
      .action = "QuarantineItems",
      .finding = finding,
      .proposer = Actor::mcp("claude-code").value(),
      .state = PlanState::awaiting_approval,
      .created_at = noon,
      .items = {PlanItem{.path = u8"C:\\Users\\Zoë\\AppData\\Local\\Temp\\a.tmp",
                         .identity = identity(1),
                         .size = 3_MiB},
                PlanItem{.path = "D:\\cache\\b.bin", .identity = identity(9), .size = 1_B}}};
}

TEST_F(RepositoryTest, FindingRoundTrips) {
  const Finding finding{.kind = "volume.fill_up",
                        .subject = "C:",
                        .severity = Severity::critical,
                        .summary = "C: fills up in 3 days",
                        .detected_at = noon};
  const Result<FindingId> id =
      in_write<FindingId>([&](Database& db) { return add_finding(db, finding); });
  ASSERT_TRUE(ok(id));

  const Result<Finding> read =
      in_write<Finding>([&](Database& db) { return get_finding(db, *id); });
  ASSERT_TRUE(ok(read));
  EXPECT_EQ(read->id, *id);
  EXPECT_EQ(read->kind, finding.kind);
  EXPECT_EQ(read->subject, finding.subject);
  EXPECT_EQ(read->severity, finding.severity);
  EXPECT_EQ(read->summary, finding.summary);
  EXPECT_EQ(read->detected_at, finding.detected_at);

  const Result<Finding> missing =
      in_write<Finding>([&](Database& db) { return get_finding(db, FindingId{999}); });
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

TEST_F(RepositoryTest, PlanRoundTripsWithItemsAcrossRestart) {
  const Result<FindingId> finding = in_write<FindingId>([&](Database& db) {
    return add_finding(db, Finding{.kind = "clean.cache", .subject = "C:", .detected_at = noon});
  });
  ASSERT_TRUE(ok(finding));
  const Plan original = cleanup_plan(*finding);
  const Result<Plan> stored = in_write<Plan>([&](Database& db) { return add_plan(db, original); });
  ASSERT_TRUE(ok(stored));
  EXPECT_NE(stored->id, PlanId{});
  EXPECT_NE(stored->items[0].id, stored->items[1].id);

  reopen();
  const Result<Plan> read = in_write<Plan>([&](Database& db) { return get_plan(db, stored->id); });

  ASSERT_TRUE(ok(read));
  EXPECT_EQ(read->action, original.action);
  EXPECT_EQ(read->finding, finding.value());
  EXPECT_EQ(read->proposer.to_string(), "mcp:claude-code");
  EXPECT_EQ(read->state, PlanState::awaiting_approval);
  EXPECT_EQ(read->created_at, noon);
  ASSERT_EQ(read->items.size(), 2U);
  for (std::size_t index = 0; index < 2; ++index) {
    EXPECT_EQ(read->items[index].id, stored->items[index].id);
    EXPECT_EQ(read->items[index].path, original.items[index].path);
    EXPECT_EQ(read->items[index].identity, original.items[index].identity);
    EXPECT_EQ(read->items[index].size, original.items[index].size);
    EXPECT_EQ(read->items[index].approval, ItemApproval::pending);
  }
}

TEST_F(RepositoryTest, PlanStateAndItemApprovalAreUpdated) {
  const Result<Plan> stored =
      in_write<Plan>([&](Database& db) { return add_plan(db, cleanup_plan(std::nullopt)); });
  ASSERT_TRUE(ok(stored));

  ASSERT_TRUE(ok(store().write([&](Database& db) -> Result<void> {
    if (Result<void> state = set_plan_state(db, stored->id, PlanState::approved); !state) {
      return state;
    }
    if (Result<void> item = set_item_approval(db, stored->items[0].id, ItemApproval::approved);
        !item) {
      return item;
    }
    return set_item_approval(db, stored->items[1].id, ItemApproval::not_approved);
  })));

  const Result<Plan> read = in_write<Plan>([&](Database& db) { return get_plan(db, stored->id); });
  ASSERT_TRUE(ok(read));
  EXPECT_EQ(read->finding, std::nullopt);
  EXPECT_EQ(read->state, PlanState::approved);
  EXPECT_EQ(read->items[0].approval, ItemApproval::approved);
  EXPECT_EQ(read->items[1].approval, ItemApproval::not_approved);

  const Result<void> missing = store().write(
      [&](Database& db) { return set_plan_state(db, PlanId{999}, PlanState::failed); });
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, Errc::not_found);
}

// The action framework records a plan change and its audit entry in one Store::write; when the
// second part fails, neither is kept.
TEST_F(RepositoryTest, PlanWriteRollsBackWithTheEnclosingTransaction) {
  const Result<void> written = store().write([&](Database& db) -> Result<void> {
    if (Result<Plan> plan = add_plan(db, cleanup_plan(std::nullopt)); !plan) {
      return tl::unexpected(plan.error());
    }
    return fail(Errc::io_error, "audit write failed");
  });

  ASSERT_FALSE(written.has_value());
  EXPECT_EQ(count_rows(store(), "plan"), 0);
  EXPECT_EQ(count_rows(store(), "plan_item"), 0);
}

TEST_F(RepositoryTest, ScheduleStateSurvivesRestart) {
  const ScheduledJob monthly{.name = "monthly-report",
                             .expression = "0 9 1 * *",
                             .last_run = noon - 24h * 30,
                             .last_result = "succeeded",
                             .next_run = noon + 24h};
  const ScheduledJob fresh{
      .name = "clean-temp", .expression = "*/15 8-18 * * 1-5", .next_run = noon + 1h};
  ASSERT_TRUE(ok(store().write([&](Database& db) -> Result<void> {
    if (Result<void> saved = save_job(db, monthly); !saved) {
      return saved;
    }
    return save_job(db, fresh);
  })));

  reopen();
  const auto load = [&](std::string_view name) {
    const Result<std::optional<ScheduledJob>> loaded =
        in_write<std::optional<ScheduledJob>>([&](Database& db) { return load_job(db, name); });
    EXPECT_TRUE(ok(loaded));
    return loaded.value_or(std::nullopt);
  };
  EXPECT_EQ(load("monthly-report"), std::optional(monthly));
  EXPECT_EQ(load("clean-temp"), std::optional(fresh));
  EXPECT_EQ(load("no-such-job"), std::nullopt);
}

TEST_F(RepositoryTest, SavingAJobAgainReplacesItsState) {
  ScheduledJob job{.name = "hourly", .expression = "0 * * * *", .next_run = noon};
  ASSERT_TRUE(ok(store().write([&](Database& db) { return save_job(db, job); })));
  job.last_run = noon;
  job.last_result = "failed";
  job.next_run = noon + 1h;
  ASSERT_TRUE(ok(store().write([&](Database& db) { return save_job(db, job); })));

  const Result<std::optional<ScheduledJob>> saved =
      in_write<std::optional<ScheduledJob>>([&](Database& db) { return load_job(db, "hourly"); });
  ASSERT_TRUE(ok(saved));
  EXPECT_EQ(saved.value_or(std::nullopt), std::optional(job));
  EXPECT_EQ(count_rows(store(), "schedule"), 1);
}

TEST_F(RepositoryTest, JobRunsAreRecordedInOrder) {
  const JobRun catch_up{
      .job = "monthly-report", .started = noon, .catch_up = true, .status = "succeeded"};
  const JobRun skipped{.job = "monthly-report",
                       .started = noon + 15min,
                       .status = "skipped",
                       .detail = "previous run still active"};
  ASSERT_TRUE(ok(store().write([&](Database& db) -> Result<void> {
    if (Result<void> added = add_job_run(db, skipped); !added) {
      return added;
    }
    return add_job_run(db, catch_up);
  })));

  const Result<std::vector<JobRun>> runs =
      in_write<std::vector<JobRun>>([&](Database& db) { return job_runs(db, "monthly-report"); });
  ASSERT_TRUE(ok(runs));
  ASSERT_EQ(runs->size(), 2U);
  EXPECT_TRUE((*runs)[0].catch_up);
  EXPECT_EQ((*runs)[0].started, noon);
  EXPECT_EQ((*runs)[0].status, "succeeded");
  EXPECT_EQ((*runs)[1].status, "skipped");
  EXPECT_FALSE((*runs)[1].catch_up);
  EXPECT_EQ((*runs)[1].detail, "previous run still active");
}

}  // namespace
}  // namespace myclean::store
