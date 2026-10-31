#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/actions/audit_log.hpp"
#include "core/actions/item_action.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/actions/schema.hpp"
#include "core/actions/store_plan_source.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/file_system.hpp"
#include "core/safety/deny_list.hpp"
#include "core/safety/safe_remove.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/plan_repository.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_file_system.hpp"

namespace myclean::actions::testing {

// 2026-10-31 12:00:00 UTC.
inline const WallTime noon =
    std::chrono::sys_days{std::chrono::October / 31 / 2026} + std::chrono::hours{12};

// A fresh folder under %TEMP% for the current test, removed afterwards.
class TempFolder {
 public:
  TempFolder()
      : path_(std::filesystem::temp_directory_path() /
              ("myclean-actions-" + std::string(current().test_suite_name()) + "-" +
               current().name())) {
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
  static const ::testing::TestInfo& current() {
    return *::testing::UnitTest::GetInstance()->current_test_info();
  }

  std::filesystem::path path_;
};

inline safety::ProtectedLocations owner_locations() {
  return {
      .windows = L"C:\\Windows",
      .profiles = L"C:\\Users",
      .owner_profile = L"C:\\Users\\owner",
      .owner_known_folders = {L"C:\\Users\\owner\\Documents", L"C:\\Users\\owner\\Desktop",
                              L"C:\\Users\\owner\\Pictures", L"C:\\Users\\owner\\Videos",
                              L"C:\\Users\\owner\\Music"},
      .myclean = {L"C:\\ProgramData\\MyClean", L"C:\\Program Files\\MyClean"},
  };
}

// The action framework wired as the service wires it: a store with the action tables, the
// plan source over it, SafeRemove over a fake file system, the policy engine and quarantine.
class ActionsTest : public ::testing::Test {
 protected:
  ActionsTest() { open(); }

  // A restart: everything is rebuilt over the same database file.
  void reopen() {
    quarantine_.reset();
    engine_.reset();
    safe_remove_.reset();
    plans_.reset();
    store_.reset();
    open();
  }

  // Stores a plan over `paths` (identity and size as the fake file system has them now).
  Plan plan_of(const std::vector<std::filesystem::path>& paths, const Actor& proposer,
               const std::string& action = "DeleteItems") {
    Plan plan{.action = action, .proposer = proposer, .created_at = clock_.now()};
    for (const std::filesystem::path& item : paths) {
      const os::FileStat stat = fs_.stat(item).value();
      plan.items.push_back(
          PlanItem{.path = item, .identity = stat.identity, .size = stat.size_on_disk});
    }
    return plan;
  }

  Plan submit(const std::vector<std::filesystem::path>& paths, AutomationLevel level,
              const Actor& proposer, const std::string& action = "DeleteItems") {
    return engine_->submit(plan_of(paths, proposer, action), level).value();
  }

  [[nodiscard]] std::vector<LogEntry> log(PlanId plan) {
    std::vector<LogEntry> entries;
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      Result<std::vector<LogEntry>> read = plan_log(db, plan);
                      if (!read) {
                        return tl::unexpected(read.error());
                      }
                      entries = *std::move(read);
                      return {};
                    })
                    .has_value());
    return entries;
  }

  [[nodiscard]] Plan stored(PlanId plan) {
    Result<Plan> found = fail(Errc::io_error, "not read");
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      found = store::get_plan(db, plan);
                      return {};
                    })
                    .has_value());
    return found.value();
  }

  [[nodiscard]] std::int64_t count(const std::string& sql) {
    std::int64_t value = -1;
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      Result<store::Statement> query = db.prepare(sql);
                      if (!query) {
                        return tl::unexpected(query.error());
                      }
                      if (Result<bool> row = query->step(); !row) {
                        return tl::unexpected(row.error());
                      }
                      value = query->column_int(0);
                      return {};
                    })
                    .has_value());
    return value;
  }

  [[nodiscard]] Environment environment() {
    return {.store = store_.get(),
            .file_system = &fs_,
            .safe_remove = &*safe_remove_,
            .clock = &clock_};
  }

  store::Store& store() { return *store_; }
  fakes::FakeClock& clock() { return clock_; }
  fakes::FakeFileSystem& fs() { return fs_; }
  PolicyEngine& engine() { return *engine_; }
  Quarantine& quarantine() { return *quarantine_; }

 private:
  void open() {
    const std::vector<store::MigrationSet> migrations = {action_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, migrations).value();
    plans_ = std::make_unique<StorePlanSource>(*store_);
    safe_remove_.emplace(safety::SafeRemove::create(fs_, *plans_, owner_locations()).value());
    engine_ = std::make_unique<PolicyEngine>(*store_, clock_);
    quarantine_ = std::make_unique<Quarantine>(environment(), *engine_);
  }

  TempFolder folder_;
  fakes::FakeClock clock_{noon};
  fakes::FakeFileSystem fs_;
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<StorePlanSource> plans_;
  std::optional<safety::SafeRemove> safe_remove_;
  std::unique_ptr<PolicyEngine> engine_;
  std::unique_ptr<Quarantine> quarantine_;
};

}  // namespace myclean::actions::testing
