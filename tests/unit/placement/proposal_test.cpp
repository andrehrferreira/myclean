#include "core/placement/proposal.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/actions/audit_log.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/schema.hpp"
#include "core/model/actor.hpp"
#include "core/model/ids.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/placement/system_space.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_file_system.hpp"
#include "placement_fakes.hpp"

namespace myclean::placement {
namespace {

using actions::AutomationLevel;
using actions::PolicyEngine;

// A fresh folder of the test sandbox (%TEMP%\myclean-it), removed afterwards.
class Folder {
 public:
  Folder()
      : path_(std::filesystem::temp_directory_path() / "myclean-it" /
              ("placement-" +
               std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  Folder(const Folder&) = delete;
  Folder& operator=(const Folder&) = delete;
  Folder(Folder&&) = delete;
  Folder& operator=(Folder&&) = delete;
  ~Folder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// The policy engine over a store in the test sandbox.
class ProposalTest : public ::testing::Test {
 protected:
  ProposalTest() {
    open();
    system_.hibernation_state = {.mode = HibernationMode::full,
                                 .file = LR"(C:\hiberfil.sys)",
                                 .file_size = Bytes{50ULL << 30U},
                                 .memory = Bytes{128ULL << 30U}};
  }

  // A restart: the store and the engine are rebuilt over the same database file.
  void open() {
    engine_.reset();
    store_.reset();
    const std::vector<store::MigrationSet> migrations = {actions::action_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, migrations).value();
    engine_ = std::make_unique<PolicyEngine>(*store_, clock_);
  }

  fakes::FakeClock& clock() { return clock_; }
  store::Store& store() { return *store_; }
  PolicyEngine& engine() { return *engine_; }
  testing::FakeSystemSpace& system() { return system_; }
  testing::FakeProcessRunner& runner() { return runner_; }

 private:
  Folder folder_;
  fakes::FakeClock clock_{WallTime{std::chrono::sys_days{std::chrono::October / 31 / 2026}}};
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<PolicyEngine> engine_;
  testing::FakeSystemSpace system_;
  testing::FakeProcessRunner runner_;
};

TEST_F(ProposalTest, AnAutomaticLevelStillWaitsForTheOwnersApproval) {
  HibernationAction action{system(), runner(), HibernationMode::reduced};
  Result<Plan> plan = plan_of(action, "SetHibernation", Actor::scheduler(), clock().now());
  ASSERT_TRUE(plan.has_value());
  ASSERT_EQ(plan->items.size(), 1U);
  EXPECT_EQ(plan->items[0].path, LR"(C:\hiberfil.sys)");

  const Result<Plan> proposed = propose(engine(), *std::move(plan), AutomationLevel::automatic);

  ASSERT_TRUE(proposed.has_value()) << proposed.error().message;
  EXPECT_EQ(proposed->state, PlanState::awaiting_approval);
  EXPECT_TRUE(runner().commands.empty());
}

TEST_F(ProposalTest, ObserveProducesNoPlan) {
  HibernationAction action{system(), runner(), HibernationMode::reduced};
  const Result<Plan> proposed =
      propose(engine(), *plan_of(action, "SetHibernation", Actor::scheduler(), clock().now()),
              AutomationLevel::observe);
  ASSERT_FALSE(proposed.has_value());
  EXPECT_EQ(proposed.error().code, Errc::refused);
}

TEST_F(ProposalTest, AnApprovedRelocationRunsAndIsUndoneAfterARestart) {
  testing::FakeUserEnvironment environment;
  testing::FakeTextFiles files;
  fakes::FakeFileSystem file_system;
  testing::FakeTreeCopier copier{file_system};
  testing::FakeDriveFacts drives;
  drives.list() = testing::owner_volumes();
  for (const TargetVolume& volume : drives.list()) {
    file_system.add_directory(volume.root);
  }
  environment.put("TEMP", R"(%USERPROFILE%\AppData\Local\Temp)", true);
  environment.put("TMP", R"(%USERPROFILE%\AppData\Local\Temp)", true);
  const Tools tools{.environment = &environment,
                    .files = &files,
                    .copier = &copier,
                    .file_system = &file_system,
                    .drives = &drives};
  const Relocation relocation{.kind = CacheKind::temp, .target = LR"(D:\Temp)"};

  RelocateAction action{tools, relocation};
  const Result<Plan> proposed =
      propose(engine(), *plan_of(action, "RelocateCache", Actor::owner(), clock().now()),
              AutomationLevel::suggest);
  ASSERT_TRUE(proposed.has_value());
  const std::vector<PlanItemId> approved = {proposed->items.at(0).id};
  ASSERT_EQ(engine().approve(proposed->id, approved, Actor::owner()), PlanState::approved);
  const Result<actions::RunReport> report =
      engine().execute(proposed->id, action, Actor::owner(), false);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_EQ(report->state, PlanState::verified);
  EXPECT_EQ(environment.value("TEMP"), (EnvValue{.text = R"(D:\Temp)", .expandable = true}));

  // After a restart the undo data comes back from undo_blob and a rebuilt action reverses it.
  open();
  Result<Undo> stored = fail(Errc::not_found, "not read");
  ASSERT_TRUE(store()
                  .read([&](store::Database& db) -> Result<void> {
                    stored = actions::load_undo(db, proposed->id);
                    return {};
                  })
                  .has_value());
  ASSERT_TRUE(stored.has_value()) << stored.error().message;
  RelocateAction rebuilt{tools, relocation};
  const Result<void> undone = rebuilt.undo(*stored);
  ASSERT_TRUE(undone.has_value()) << undone.error().message;
  const EnvValue temp{.text = R"(%USERPROFILE%\AppData\Local\Temp)", .expandable = true};
  EXPECT_EQ(environment.value("TEMP"), temp);
  EXPECT_EQ(environment.value("TMP"), temp);
}

}  // namespace
}  // namespace myclean::placement
