// Game detection and game mode in the governor (governor spec, "Game Detection", "Game Mode"),
// over FakeJobs.

#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/games/fullscreen.hpp"
#include "core/games/library_roots.hpp"
#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/game_mode.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/telemetry/metrics.hpp"
#include "governing_test_support.hpp"
#include "telemetry/fake_sources.hpp"

namespace myclean::governing {
namespace {

using namespace myclean::literals;
using governor::JobRole;
using governor::ProcessKey;
using testing::jobs_of;
using testing::Machine;

constexpr const char* cargo_path = R"(C:\Users\owner\.cargo\bin\cargo.exe)";
constexpr const char* game_path = R"(E:\Jogos\Title\title.exe)";

void with_owner_root(Machine& machine) {
  machine.games().sources.roots = {games::LibraryRoot{.path = R"(E:\Jogos)"}};
}

// Scenario "Game starts during a build" and "Game detected from library root".
TEST(GovernorGames, GameLowersTheBuildCapUntilItExits) {
  Machine machine;
  with_owner_root(machine);
  machine.start();
  machine.spawn(100, cargo_path);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  const os::JobRef parent = jobs_of(machine.store(), JobRole::class_job).at(0).ref;
  machine.second();
  EXPECT_EQ(machine.os().job_limits(tree).cpu_cap_percent, 75.0);

  const ProcessKey game = machine.spawn(500, game_path);
  machine.second();
  EXPECT_EQ(machine.os().job_limits(tree).cpu_cap_percent, 40.0);
  EXPECT_EQ(machine.os().job_limits(parent).cpu_cap_percent, 40.0);
  const nlohmann::json status = machine.governor().status();
  EXPECT_EQ(status.at("game_mode").at("active"), true);
  EXPECT_EQ(status.at("game_mode").at("build_cpu_percent"), 40);
  ASSERT_EQ(status.at("game_mode").at("games").size(), 1U);
  EXPECT_EQ(status.at("game_mode").at("games").at(0).at("pid"), 500);
  EXPECT_EQ(status.at("game_mode").at("games").at(0).at("reason"), "library_root");

  // A build started during game mode is capped at 40 % from its creation.
  machine.spawn(110, R"(C:\VS\MSBuild.exe)");
  for (const JobRecord& job : jobs_of(machine.store(), JobRole::tree_job)) {
    EXPECT_EQ(machine.os().job_limits(job.ref).cpu_cap_percent, 40.0);
  }

  machine.exit(game);
  machine.second();
  for (const JobRecord& job : jobs_of(machine.store(), JobRole::tree_job)) {
    EXPECT_EQ(machine.os().job_limits(job.ref).cpu_cap_percent, 75.0);
  }
  EXPECT_EQ(machine.os().job_limits(parent).cpu_cap_percent, 75.0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'game_mode'"), 2);
}

// Scenario "Override off".
TEST(GovernorGames, OverrideOffKeepsTheCapButStillProtectsTheGame) {
  Machine machine;
  with_owner_root(machine);
  machine.start();
  machine.spawn(100, cargo_path);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  EXPECT_FALSE(machine.governor().set_override(governor::GameModeOverride::off).active);
  const ProcessKey game = machine.spawn(500, game_path, std::nullopt, true, 95.0, 8_GiB);
  machine.seconds(5);
  EXPECT_EQ(machine.os().job_limits(tree).cpu_cap_percent, 75.0);
  EXPECT_EQ(machine.governor().status().at("game_mode").at("games").size(), 1U);

  // Under pressure the ladder acts on the build tree, never on the game.
  machine.metrics()[std::string(telemetry::metric::cpu_utility)] = 95.0;
  machine.seconds(60);
  EXPECT_EQ(machine.process(game).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(game).suspend_count, 0U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE root_pid = 500"), 0);
}

// Scenario "Override on".
TEST(GovernorGames, OverrideOnActivatesGameModeWithoutAGame) {
  Machine machine;
  machine.start();
  machine.spawn(100, cargo_path);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  const governor::GameMode mode = machine.governor().set_override(governor::GameModeOverride::on);
  EXPECT_TRUE(mode.active);
  EXPECT_EQ(mode.build_cpu_percent, 40);
  EXPECT_EQ(machine.os().job_limits(tree).cpu_cap_percent, 40.0);
  EXPECT_EQ(machine.governor().status().at("game_mode").at("override"), "on");
}

// Scenario "Store Python is not a game": a packaged GameConfigStore entry is ignored; a
// path-matched one makes a game.
TEST(GovernorGames, OnlyPathMatchedGameConfigStoreEntriesMakeGames) {
  Machine machine;
  machine.games().sources.game_config = {
      games::GameConfigEntry{
          .type = 2, .matched_exe_full_path = R"(C:\Program Files\WindowsApps\Python\python.exe)"},
      games::GameConfigEntry{.type = 1, .matched_exe_full_path = R"(D:\Indie\indie.exe)"}};
  machine.start();
  machine.spawn(700, R"(C:\Program Files\WindowsApps\Python\python.exe)");
  machine.spawn(701, R"(D:\Indie\indie.exe)");
  machine.second();
  const nlohmann::json games = machine.governor().status().at("game_mode").at("games");
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games.at(0).at("pid"), 701);
  EXPECT_EQ(games.at(0).at("reason"), "game_config_store");
}

// Game sources are read again when the owner logs on (the tray's first state after a pause).
TEST(GovernorGames, OwnerLogonRescansTheGameSources) {
  Machine machine;
  machine.start();
  machine.games().sources.roots = {games::LibraryRoot{.path = R"(E:\Jogos)"}};
  machine.spawn(500, game_path, std::nullopt, false);
  machine.second();
  EXPECT_TRUE(machine.governor().status().at("game_mode").at("games").empty());

  machine.governor().set_user_state(
      UserState{.state = games::NotificationState::accepts_notifications, .foreground_pid = 0});
  machine.second();
  EXPECT_EQ(machine.governor().status().at("game_mode").at("games").size(), 1U);
}

// Scenario "Fullscreen with GPU use": the foreground fullscreen process keeps the 3D engine busy
// for 10 s. GPU use is sampled only while it is the candidate.
TEST(GovernorGames, FullscreenForegroundWithGpuUseBecomesAGame) {
  telemetry::testing::FakeCounters counters;
  counters.set(games::gpu_engine_counter,
               {telemetry::CounterValue{
                   .instance = "pid_800_luid_0x00000000_0x0000D1B5_phys_0_eng_0_engtype_3D",
                   .value = 35.0}});
  Machine machine;
  machine.gpu() = &counters;
  machine.start();
  machine.spawn(800, R"(C:\Unlisted\game.exe)", std::nullopt, false);
  machine.second();
  EXPECT_EQ(counters.collects(), 0);  // no candidate yet: no GPU query
  for (int second = 0; second < 15; ++second) {
    machine.governor().set_user_state(
        UserState{.state = games::NotificationState::busy, .foreground_pid = 800});
    machine.second();
  }
  const nlohmann::json games = machine.governor().status().at("game_mode").at("games");
  ASSERT_EQ(games.size(), 1U);
  EXPECT_EQ(games.at(0).at("reason"), "fullscreen_gpu");
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'game_detected' AND "
                          "root_pid = 800"),
            1);
  // Detected: no more GPU queries.
  const int collects = counters.collects();
  machine.second();
  EXPECT_EQ(counters.collects(), collects);
}

// Scenario "Fullscreen without GPU use": the fullscreen state alone never makes a game.
TEST(GovernorGames, FullscreenWithoutGpuSamplesIsNoGame) {
  Machine machine;
  machine.start();
  machine.spawn(800, R"(C:\Video\player.exe)", std::nullopt, false);
  for (int second = 0; second < 30; ++second) {
    machine.governor().set_user_state(UserState{
        .state = games::NotificationState::running_d3d_full_screen, .foreground_pid = 800});
    machine.second();
  }
  EXPECT_TRUE(machine.governor().status().at("game_mode").at("games").empty());
  EXPECT_EQ(machine.governor().status().at("game_mode").at("active"), false);
}

}  // namespace
}  // namespace myclean::governing
