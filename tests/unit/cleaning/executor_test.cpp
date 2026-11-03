// The executor's plans for relocation recipes (cleaning/executor.hpp, propose_optimization with a
// recipe), over a real store in a temporary folder and the placement fakes.

#include "core/cleaning/executor.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "actions/actions_test_support.hpp"
#include "cleanup/fakes.hpp"
#include "core/actions/policy.hpp"
#include "core/actions/quarantine.hpp"
#include "core/actions/schema.hpp"
#include "core/cleaning/records.hpp"
#include "core/model/actor.hpp"
#include "core/model/plan.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/packs/schema.hpp"
#include "core/placement/recipe.hpp"
#include "core/placement/relocation.hpp"
#include "core/placement/settings.hpp"
#include "core/store/database.hpp"
#include "core/store/migrations.hpp"
#include "core/store/plan_repository.hpp"
#include "core/store/store.hpp"
#include "fakes/fake_clock.hpp"
#include "fakes/fake_file_system.hpp"
#include "placement/placement_fakes.hpp"

namespace myclean::cleaning {
namespace {

using Json = nlohmann::json;
using packs::SettingKind;

packs::RelocationRecipe distro_recipe() {
  return {.id = "wsl-docker/distro",
          .label = "WSL distribution disk (ext4.vhdx)",
          .locations = {R"(%LOCALAPPDATA%\wsl\*\ext4.vhdx)"},
          .current = {.kind = SettingKind::registry,
                      .text = R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\*\BasePath)",
                      .name = "DistributionName",
                      .where = packs::InstanceFilter{.value = "Flags", .mask = 8, .equals = 8}},
          .target_kind = SettingKind::command,
          .command = "wsl --manage {name} --move {target}",
          .content = packs::ContentMode::move,
          .needs = {"wslhost.exe"},
          .skip_names = {"docker-desktop"},
          .undo = packs::UndoKind::command,
          .note = "Stop the distribution first."};
}

packs::RelocationRecipe models_recipe() {
  return {.id = "ai-tools/ollama-models",
          .label = "Ollama models (OLLAMA_MODELS)",
          .locations = {R"(%USERPROFILE%\.ollama\models)"},
          .current = {.kind = SettingKind::env, .text = "OLLAMA_MODELS"},
          .target_kind = SettingKind::env,
          .content = packs::ContentMode::move,
          .undo = packs::UndoKind::restore};
}

class ExecutorRecipeTest : public ::testing::Test {
 protected:
  ExecutorRecipeTest() {
    drives_.list() = placement::testing::owner_volumes();
    const std::vector<store::MigrationSet> sets = {actions::action_migrations(),
                                                   cleaning_migrations()};
    store_ = store::Store::open(folder_.path() / "myclean.db", clock_, sets).value();
    plans_ = std::make_unique<actions::StorePlanSource>(*store_);
    safe_remove_.emplace(
        safety::SafeRemove::create(fs_, *plans_, actions::testing::owner_locations()).value());
    engine_ = std::make_unique<actions::PolicyEngine>(*store_, clock_);
    const actions::Environment environment{.store = store_.get(),
                                           .file_system = &fs_,
                                           .safe_remove = &*safe_remove_,
                                           .clock = &clock_};
    quarantine_ = std::make_unique<actions::Quarantine>(environment, *engine_);
    executor_ = std::make_unique<Executor>(environment, *engine_, *quarantine_,
                                           Probes{.commands = &runner_,
                                                  .system = &system_,
                                                  .tools = &tools_,
                                                  .drives = &drives_,
                                                  .owner_tools = &owner_tools_},
                                           actions::testing::owner_locations(), ExecutorOptions{});
  }

  Result<Plan> propose(Optimization request) {
    request.kind = "relocate";
    return executor_->propose_optimization(request, Actor::owner());
  }

  PlanSpec spec(const Plan& plan) {
    Result<PlanSpec> found = fail(Errc::not_found, "not read");
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      found = spec_of(db, store::get_plan(db, plan.id).value());
                      return {};
                    })
                    .has_value());
    return found.value();
  }

  placement::testing::FakeDriveFacts& drives() { return drives_; }
  placement::testing::FakeRegistry& registry() { return registry_; }
  placement::testing::FakeProcessTable& processes() { return processes_; }
  placement::testing::FakeSystemSpace& system() { return system_; }
  Executor& executor() { return *executor_; }
  PlanState state_of(PlanId id) {
    std::optional<PlanState> state;
    EXPECT_TRUE(store_
                    ->read([&](store::Database& db) -> Result<void> {
                      state = store::get_plan(db, id).value().state;
                      return {};
                    })
                    .has_value());
    return state.value_or(PlanState::failed);
  }

 private:
  actions::testing::TempFolder folder_;
  fakes::FakeClock clock_{std::chrono::sys_days{std::chrono::October / 31 / 2026}};
  fakes::FakeFileSystem fs_;
  placement::testing::FakeSystemSpace system_;
  placement::testing::FakeProcessRunner tools_;
  placement::testing::FakeDriveFacts drives_;
  placement::testing::FakeUserEnvironment user_environment_;
  placement::testing::FakeTextFiles text_files_;
  placement::testing::FakeRegistry registry_;
  placement::testing::FakeProcessTable processes_;
  cleanup::testing::FakeRunner runner_;
  placement::Tools owner_tools_{.environment = &user_environment_,
                                .files = &text_files_,
                                .file_system = &fs_,
                                .registry = &registry_,
                                .processes = &processes_};
  std::unique_ptr<store::Store> store_;
  std::unique_ptr<actions::StorePlanSource> plans_;
  std::optional<safety::SafeRemove> safe_remove_;
  std::unique_ptr<actions::PolicyEngine> engine_;
  std::unique_ptr<actions::Quarantine> quarantine_;
  std::unique_ptr<Executor> executor_;
};

TEST_F(ExecutorRecipeTest, ARecipeBecomesAnApprovalOnlyUserPlanCarryingTheWholeRecipe) {
  const Result<Plan> plan = propose({.recipe = distro_recipe(), .instance = "Ubuntu-24.04"});
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->action, "RelocateRecipe");
  EXPECT_EQ(plan->state, PlanState::awaiting_approval);
  ASSERT_EQ(plan->items.size(), 1U);
  EXPECT_EQ(plan->items[0].path, L"relocate:wsl-docker/distro:Ubuntu-24.04");

  const PlanSpec stored = spec(*plan);
  EXPECT_EQ(stored.kind, PlanKind::relocate);
  EXPECT_EQ(stored.tier, Tier::ask);
  EXPECT_EQ(stored.scope, ExecutionContext::user);
  const std::string target = R"(D:\Relocated\wsl-docker\distro\Ubuntu-24.04)";
  EXPECT_EQ(stored.title, "Relocate WSL distribution disk (ext4.vhdx) Ubuntu-24.04 to " + target);
  EXPECT_EQ(stored.commands,
            std::vector<std::string>{"wsl --manage Ubuntu-24.04 --move " + target});
  EXPECT_EQ(stored.undo, "moves it back the same way to its previous folder");
  EXPECT_EQ(stored.warnings.back(), "Stop the distribution first.");
  EXPECT_EQ(stored.params, (Json{{"recipe", placement::recipe_json(distro_recipe())},
                                 {"instance", "Ubuntu-24.04"},
                                 {"target", target},
                                 {"content", "move"}}));
}

TEST_F(ExecutorRecipeTest, ARecipeWithoutInstancesGoesToItsOwnFolderOnTheChosenDrive) {
  const Result<Plan> plan =
      propose({.target = "F", .content = "regenerate", .recipe = models_recipe()});
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  EXPECT_EQ(plan->items[0].path, L"relocate:ai-tools/ollama-models");
  const PlanSpec stored = spec(*plan);
  EXPECT_EQ(stored.params.at("target"), R"(F:\Relocated\ai-tools\ollama-models)");
  EXPECT_EQ(stored.params.at("content"), "regenerate");
  EXPECT_EQ(stored.undo, "restores OLLAMA_MODELS to its exact previous value");
}

TEST_F(ExecutorRecipeTest, RefusesAMissingOrNeedlessInstanceASkippedOneAndAnUnhealthyTarget) {
  EXPECT_EQ(propose({.recipe = distro_recipe()}).error().code, Errc::invalid_argument);
  EXPECT_EQ(propose({.recipe = models_recipe(), .instance = "x"}).error().code,
            Errc::invalid_argument);
  EXPECT_EQ(propose({.recipe = distro_recipe(), .instance = "Docker-Desktop"}).error().code,
            Errc::refused);
  drives().at(LR"(D:\)").healthy = false;
  EXPECT_EQ(propose({.target = "D", .recipe = models_recipe()}).error().code, Errc::invalid_state);
}

class ReclaimSystemSpaceTest : public ExecutorRecipeTest {
 protected:
  // C: at 5 % free, hibernation full and a system-managed page file on C:.
  void low_system_volume() {
    using namespace literals;
    drives().at(LR"(C:\)").free = 47_GiB;
    system().hibernation_state = {.mode = placement::HibernationMode::full,
                                  .file = LR"(C:\hiberfil.sys)",
                                  .file_size = 51_GiB,
                                  .memory = 128_GiB};
    system().page_file_state = {
        .config = {.automatic = true},
        .in_use = {placement::PageFileUse{.path = LR"(C:\pagefile.sys)", .allocated = 87_GiB}}};
  }
};

TEST_F(ReclaimSystemSpaceTest, LowSystemVolumeGetsAutomaticHibernationAndPageFilePlansOnce) {
  low_system_volume();

  const Result<std::vector<PlanId>> plans = executor().reclaim_system_space();
  ASSERT_TRUE(plans.has_value()) << plans.error().message;
  ASSERT_EQ(plans->size(), 2U);
  for (const PlanId id : *plans) {
    EXPECT_EQ(state_of(id), PlanState::approved);
  }
  const Result<std::vector<PlanId>> again = executor().reclaim_system_space();
  ASSERT_TRUE(again.has_value());
  EXPECT_TRUE(again->empty()) << "their plans are still open";

  // powercfg runs without effect here, so reduced hibernation is still recommended once the
  // plan has run: it is not tried again within the week.
  ASSERT_TRUE(executor().execute(plans->front()).has_value());
  EXPECT_EQ(system().hibernation_state.mode, placement::HibernationMode::full);
  EXPECT_TRUE(executor().reclaim_system_space().value().empty());
}

TEST_F(ReclaimSystemSpaceTest, SystemVolumeWithRoomOrSettingsAlreadyReducedGetNothing) {
  using namespace literals;
  low_system_volume();
  drives().at(LR"(C:\)").free = 200_GiB;
  EXPECT_TRUE(executor().reclaim_system_space().value().empty());

  drives().at(LR"(C:\)").free = 47_GiB;
  system().hibernation_state.mode = placement::HibernationMode::reduced;
  system().page_file_state.config = {
      .automatic = false,
      .files = {placement::PageFile{
                    .path = LR"(C:\pagefile.sys)", .initial_mb = 4096, .maximum_mb = 4096},
                placement::PageFile{.path = LR"(D:\pagefile.sys)"}}};
  EXPECT_TRUE(executor().reclaim_system_space().value().empty());
}

// The WSL 2 distribution recipe as wsl-docker ships it: automatic.
packs::RelocationRecipe automatic_distro_recipe() {
  return {.id = "wsl-docker/distro",
          .label = "WSL distribution disk (ext4.vhdx)",
          .locations = {R"(%LOCALAPPDATA%\wsl\*\ext4.vhdx)"},
          .current = {.kind = SettingKind::registry,
                      .text = R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss\*\BasePath)",
                      .name = "DistributionName",
                      .where = packs::InstanceFilter{.value = "Flags", .mask = 8, .equals = 8}},
          .target_kind = SettingKind::command,
          .command = "wsl --manage {name} --move {target}",
          .content = packs::ContentMode::move,
          .needs = {"wsl.exe", "wslhost.exe"},
          .skip_names = {"docker-desktop"},
          .undo = packs::UndoKind::command,
          .automatic = true};
}

TEST_F(ReclaimSystemSpaceTest, IdleDistributionsOnTheLowSystemVolumeMoveAutomatically) {
  low_system_volume();
  const std::string lxss = R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Lxss)";
  const auto distro = [&](const std::string& guid, const std::string& name,
                          const std::string& path) {
    registry().put(lxss + "\\" + guid, "DistributionName", placement::RegistryValue{.text = name});
    registry().put(lxss + "\\" + guid, "BasePath", placement::RegistryValue{.text = path});
    registry().put(lxss + "\\" + guid, "Flags",
                   placement::RegistryValue{.text = "15", .type = placement::RegistryType::dword});
  };
  distro("{a}", "Ubuntu", R"(C:\Users\owner\AppData\Local\wsl\{a})");
  distro("{b}", "Arch", R"(D:\WSL\Arch)");
  const std::vector<packs::RelocationRecipe> recipes = {automatic_distro_recipe()};

  // While WSL runs, nothing of it moves.
  processes().images = {"wslhost.exe"};
  ASSERT_EQ(executor().reclaim_system_space(recipes).value().size(), 2U);  // the two settings

  processes().images = {};
  const Result<std::vector<PlanId>> moved = executor().reclaim_system_space(recipes);
  ASSERT_TRUE(moved.has_value()) << moved.error().message;
  ASSERT_EQ(moved->size(), 1U);  // Ubuntu, on C:; Arch is on D: already
  EXPECT_EQ(state_of(moved->front()), PlanState::approved);
  EXPECT_TRUE(executor().reclaim_system_space(recipes).value().empty());
}

}  // namespace
}  // namespace myclean::cleaning
