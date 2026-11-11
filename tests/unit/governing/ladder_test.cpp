// The ladder through the executor (governor spec, "Escalation Ladder", "Every Step Recorded",
// "Fail-Open on Clean Stop", "Rate-Limited Owner Notifications", "Pause Governor"), observe mode
// and the owner's choices, over FakeJobs.

#include "core/governor/ladder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/governing/records.hpp"
#include "core/governor/commands.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/governor/recovery.hpp"
#include "core/ipc/governor.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/store/database.hpp"
#include "core/telemetry/metrics.hpp"
#include "governing_test_support.hpp"

namespace myclean::governing {
namespace {

using namespace myclean::literals;
using governor::JobRole;
using governor::ProcessKey;
using testing::jobs_of;
using testing::Machine;

constexpr const char* bigjob_path = R"(C:\Tools\bigjob.exe)";
constexpr const char* cargo_path = R"(C:\Users\owner\.cargo\bin\cargo.exe)";

// CPU pressure: the machine at 95 %.
void cpu_pressure(Machine& machine) {
  machine.metrics()[std::string(telemetry::metric::cpu_utility)] = 95.0;
}

constexpr double gib = 1024.0 * 1024.0 * 1024.0;

// Memory pressure as well: commit at 140 GiB of 150 GiB. A suspend or kill needs it.
void memory_pressure(Machine& machine) {
  cpu_pressure(machine);
  machine.metrics()[std::string(telemetry::metric::memory_commit)] = 140.0 * gib;
}

// Every signal back inside its exit threshold.
void calm(Machine& machine) {
  machine.metrics()[std::string(telemetry::metric::cpu_utility)] = 30.0;
  machine.metrics()[std::string(telemetry::metric::memory_commit)] = 40.0 * gib;
}

std::optional<GovernorEvent> find_event(Machine& machine, EventKind kind,
                                        std::optional<governor::Step> step) {
  for (const GovernorEvent& event : machine.events()) {
    if (event.kind == kind && event.step == step) {
      return event;
    }
  }
  return std::nullopt;
}

// Scenario "Ladder escalates with cooldown" and "Ladder step logged".
TEST(GovernorLadder, StepsAreAppliedWithCooldownAndRecorded) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  const ProcessKey worker = machine.spawn(301, R"(C:\Tools\worker.exe)", bigjob, true, 30.0, 1_GiB);
  cpu_pressure(machine);

  machine.seconds(14);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  machine.second();  // the 15th second above 90 %: pressure, step 1
  for (const ProcessKey& member : {bigjob, worker}) {
    EXPECT_EQ(machine.process(member).priority, os::PriorityClass::below_normal);
    EXPECT_EQ(machine.process(member).memory_priority, os::MemoryPriority::low);
  }
  machine.seconds(9);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  machine.second();  // 10 s later: step 2
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::on);
  EXPECT_EQ(machine.process(worker).eco_qos, os::EcoQos::on);

  const std::optional<GovernorEvent> ecoqos =
      find_event(machine, EventKind::apply_step, governor::Step::ecoqos);
  ASSERT_TRUE(ecoqos.has_value());
  const GovernorEvent event = ecoqos.value_or(GovernorEvent{});
  EXPECT_EQ(event.root, bigjob);
  EXPECT_EQ(event.root_image, "bigjob.exe");
  EXPECT_EQ(event.reason, governor::Reason::cpu);
  EXPECT_EQ(event.actor, Actor::governor());
  EXPECT_EQ(event.members, 2U);
  ASSERT_TRUE(event.metrics.has_value());
  const governor::PressureSample metrics = event.metrics.value_or(governor::PressureSample{});
  EXPECT_EQ(metrics.cpu_percent, 95.0);
  EXPECT_EQ(metrics.available, 32_GiB);
  EXPECT_EQ(metrics.commit, 40_GiB);
  EXPECT_EQ(metrics.commit_limit, 150_GiB);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE event = 'GovernorStep' AND "
                          "outcome = 'apply_step' AND reason = 'cpu' AND actor = 'governor' AND "
                          "detail LIKE 'ecoqos bigjob.exe pid 300%' AND metrics LIKE 'cpu=95.0%'"),
            1);

  // Step 3: a cap job of its own (the tree is in no MyClean job), CPU 50 %, commit + 25 %.
  machine.seconds(10);
  const std::vector<JobRecord> caps = jobs_of(machine.store(), JobRole::cap_job);
  ASSERT_EQ(caps.size(), 1U);
  EXPECT_EQ(caps[0].ref.job_class, "default");
  EXPECT_EQ(machine.os().job_limits(caps[0].ref).cpu_cap_percent, 50.0);
  EXPECT_EQ(machine.os().job_limits(caps[0].ref).commit_cap, Bytes{(3_GiB).count() / 4 * 5});
  EXPECT_EQ(machine.process(worker).jobs, machine.process(bigjob).jobs);

  // CPU pressure alone stops at the cap; with a memory shortage, step 4: suspend, persisted
  // first, with a notification for the owner.
  machine.seconds(30);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 0U);
  memory_pressure(machine);
  machine.second();
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
  EXPECT_EQ(machine.process(worker).suspend_count, 1U);
  EXPECT_EQ(machine.count("SELECT SUM(count) FROM governor_suspend"), 2);
  const nlohmann::json status = machine.governor().status();
  ASSERT_FALSE(status.at("notifications").empty());
  EXPECT_EQ(status.at("notifications").back().at("step"), "suspend");
  EXPECT_EQ(status.at("notifications").back().at("root").at("pid"), 300);
  EXPECT_EQ(status.at("trees").at(0).at("step"), "suspend");

  // Scenario "Kill off by default": the suspended tree stays suspended.
  machine.seconds(30);
  EXPECT_TRUE(machine.process(bigjob).alive);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
}

// Scenario "Throttled tree restored" and "Suspended tree waits for the owner".
TEST(GovernorLadder, ClearedPressureRollsBackButKeepsSuspends) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  machine.os()
      .set_priority(os::ProcessKey{.pid = bigjob.pid, .created = bigjob.created},
                    os::PriorityClass::above_normal)
      .value();
  cpu_pressure(machine);
  machine.seconds(35);  // priority, ecoqos, cap job
  const os::JobRef cap = jobs_of(machine.store(), JobRole::cap_job).at(0).ref;
  machine.metrics()[std::string(telemetry::metric::cpu_utility)] = 30.0;
  machine.seconds(61);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::above_normal);
  EXPECT_EQ(machine.process(bigjob).memory_priority, os::MemoryPriority::normal);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  EXPECT_FALSE(machine.os().job_limits(cap).cpu_cap_percent.has_value());
  EXPECT_FALSE(machine.os().job_exists(cap));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE outcome = 'roll_back' AND "
                          "reason = 'pressure_cleared'"),
            1);

  // A suspended tree is not resumed when pressure clears.
  memory_pressure(machine);
  machine.seconds(45);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
  calm(machine);
  machine.seconds(61);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
  EXPECT_EQ(machine.count("SELECT SUM(count) FROM governor_suspend"), 1);
}

// Step 3 on a tree in its MyClean tree job tightens that job instead of nesting another.
TEST(GovernorLadder, CapStepTightensTheTreeJobAndRollBackRestoresIt) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path, std::nullopt, true, 80.0, 4_GiB);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  cpu_pressure(machine);
  machine.seconds(35);
  EXPECT_TRUE(jobs_of(machine.store(), JobRole::cap_job).empty());
  EXPECT_EQ(machine.os().job_limits(tree).cpu_cap_percent, 50.0);
  EXPECT_EQ(machine.os().job_limits(tree).commit_cap, 5_GiB);
  machine.metrics()[std::string(telemetry::metric::cpu_utility)] = 30.0;
  machine.seconds(61);
  EXPECT_EQ(machine.os().job_limits(tree).cpu_cap_percent, 75.0);
  EXPECT_EQ(machine.os().job_limits(tree).commit_cap, 96_GiB);
  static_cast<void>(cargo);
}

// Scenario "Game never receives an action" and "System process ignored".
TEST(GovernorLadder, ProtectedProcessesNeverReceiveAStep) {
  Machine machine;
  machine.games().sources.roots = {games::LibraryRoot{.path = R"(E:\Jogos)"}};
  machine.start();
  const ProcessKey game =
      machine.spawn(500, R"(E:\Jogos\Title\title.exe)", std::nullopt, true, 95.0, 8_GiB);
  const ProcessKey helper = machine.spawn(501, R"(C:\Tools\helper.exe)", game, true, 50.0, 1_GiB);
  const ProcessKey host =
      machine.spawn(600, R"(C:\Windows\System32\svchost.exe)", std::nullopt, true, 90.0, 1_GiB);
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 20.0, 1_GiB);
  cpu_pressure(machine);
  machine.seconds(60);
  for (const ProcessKey& protected_process : {game, helper, host}) {
    EXPECT_EQ(machine.process(protected_process).priority, os::PriorityClass::normal);
    EXPECT_EQ(machine.process(protected_process).suspend_count, 0U);
    EXPECT_TRUE(machine.process(protected_process).jobs.empty());
  }
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE root_pid IN (500, 501, 600) "
                          "AND kind <> 'game_detected'"),
            0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE detail LIKE '%pid 500%' OR "
                          "detail LIKE '%pid 501%' OR detail LIKE '%pid 600%'"),
            0);
}

// Observed on the reference machine: in the default class the ladder chose vmms.exe,
// NVDisplay.Container.exe and vendor services. A service (session 0) is never the governor's.
TEST(GovernorLadder, ServicesNeverReceiveAStep) {
  Machine machine;
  machine.start();
  const ProcessKey service =
      machine.spawn(700, R"(C:\Windows\System32\vmms.exe)", std::nullopt, true, 90.0, 2_GiB);
  machine.set_session(service, 0);
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 20.0, 1_GiB);
  machine.set_session(bigjob, 1);
  cpu_pressure(machine);
  machine.seconds(60);
  EXPECT_EQ(machine.process(service).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(service).suspend_count, 0U);
  EXPECT_TRUE(machine.process(service).jobs.empty());
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE root_pid = 700"), 0);
}

// Scenario "Stop releases everything".
TEST(GovernorLadder, StopLiftsEveryCapAndResumesEverySuspend) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  const os::JobRef parent = jobs_of(machine.store(), JobRole::class_job).at(0).ref;
  memory_pressure(machine);
  machine.seconds(70);  // cargo first (a build tree), then bigjob
  ASSERT_EQ(machine.process(cargo).suspend_count, 1U);
  ASSERT_EQ(machine.process(bigjob).suspend_count, 1U);
  const os::JobRef cap = jobs_of(machine.store(), JobRole::cap_job).at(0).ref;

  machine.governor().stop();
  EXPECT_EQ(machine.process(bigjob).suspend_count, 0U);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  for (const os::JobRef& job : {tree, parent, cap}) {
    EXPECT_EQ(machine.os().job_limits(job), os::JobLimits{});
    EXPECT_FALSE(machine.os().job_exists(job));
  }
  EXPECT_TRUE(machine.process(cargo).alive);
  EXPECT_EQ(machine.process(cargo).suspend_count, 0U);
  // Audit risk 5: the priority set when cargo was placed in its tree job is restored too.
  EXPECT_EQ(machine.process(cargo).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'resume' AND "
                          "reason = 'stop'"),
            2);
  machine.governor().stop();  // idempotent
}

// Observe mode (the pilot's default): nothing is applied, every decision is recorded.
TEST(GovernorLadder, ObserveModeRecordsDecisionsAndAppliesNothing) {
  Machine machine;
  machine.policy().mode = governor::GovernorMode::observe;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const ProcessKey rustc = machine.spawn(200, R"(C:\rust\rustc.exe)", cargo);
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  memory_pressure(machine);
  machine.seconds(70);  // cargo's tree first (a build tree), then bigjob's

  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
  EXPECT_TRUE(machine.process(cargo).jobs.empty());
  EXPECT_TRUE(machine.process(rustc).jobs.empty());
  EXPECT_EQ(machine.process(cargo).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 0U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log"), 0);
  // One would-be tree root (rustc would have inherited cargo's job) and every ladder step.
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job' AND "
                          "root_pid = 100 AND detail = 'observe: class_job_then_tree_job'"),
            1);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job'"), 1);
  for (const char* step : {"priority", "ecoqos", "cap_job", "suspend"}) {
    EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'apply_step' AND "
                            "root_pid = 300 AND detail = 'observe' AND step = '" +
                            std::string(step) + "'"),
              1)
        << step;
  }
  const nlohmann::json status = machine.governor().status();
  EXPECT_EQ(status.at("mode"), "observe");
  EXPECT_TRUE(status.at("notifications").empty());
}

TEST(GovernorLadder, SwitchingToObserveFailsOpen) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  machine.spawn(100, cargo_path);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  cpu_pressure(machine);
  machine.seconds(40);  // cargo's tree: priority, EcoQoS, cap; then bigjob's first step
  ASSERT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);

  machine.policy().mode = governor::GovernorMode::observe;
  machine.clock().advance(Governor::reload_interval);
  machine.second();
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  EXPECT_FALSE(machine.os().job_exists(tree));
  EXPECT_EQ(machine.governor().status().at("mode"), "observe");
}

TEST(GovernorLadder, MalformedPolicyKeepsThePreviousOne) {
  Machine machine;
  machine.start();
  machine.policy_fails() = true;
  machine.clock().advance(Governor::reload_interval);
  machine.second();
  EXPECT_EQ(machine.governor().status().at("mode"), "act");
  const std::vector<std::string> problems = machine.governor().problems();
  ASSERT_EQ(problems.size(), 1U);
  EXPECT_NE(problems[0].find("kept the previous policy"), std::string::npos);
}

// Scenario "Repeated interventions rate-limited": one notification per tree per 5 minutes.
TEST(GovernorLadder, NotificationsAreRateLimitedPerTree) {
  Machine machine;
  machine.start();
  machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  cpu_pressure(machine);
  machine.seconds(35);  // three steps in 20 s
  EXPECT_EQ(machine.governor().status().at("notifications").size(), 1U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'notify'"), 1);
}

// Scenario "Always allow".
TEST(GovernorLadder, AlwaysAllowRollsBackAndProtectsTheImage) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  cpu_pressure(machine);
  machine.seconds(25);
  ASSERT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::on);

  EXPECT_GT(machine.governor().choose(bigjob, governor::OwnerChoice::always_allow), 0U);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_allow WHERE image = 'BIGJOB.EXE'"), 1);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE actor = 'owner' AND outcome = "
                          "'add_to_allow_list' AND reason = 'always_allow'"),
            1);
  EXPECT_TRUE(machine.governor().status().at("notifications").empty());

  // A later bigjob.exe under the same pressure receives nothing.
  machine.exit(bigjob);
  const ProcessKey next = machine.spawn(310, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  machine.seconds(60);
  EXPECT_EQ(machine.process(next).priority, os::PriorityClass::normal);
}

// Scenario "Pause for 60 minutes" and "Pause expires".
TEST(GovernorLadder, PauseRollsBackResumesAndExpires) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  memory_pressure(machine);
  machine.seconds(45);
  ASSERT_EQ(machine.process(bigjob).suspend_count, 1U);

  const std::optional<WallTime> until = machine.governor().pause(std::chrono::minutes{60});
  ASSERT_TRUE(until.has_value());
  EXPECT_EQ(machine.process(bigjob).suspend_count, 0U);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_TRUE(machine.governor().status().at("paused_until").is_number());
  machine.seconds(120);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);

  machine.clock().advance(std::chrono::minutes{60});
  machine.seconds(16);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);
  EXPECT_TRUE(machine.governor().status().at("paused_until").is_null());
}

TEST(GovernorLadder, PauseZeroEndsAPause) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  ASSERT_TRUE(machine.governor().pause(std::chrono::minutes{60}).has_value());
  EXPECT_FALSE(machine.governor().pause(std::chrono::minutes{0}).has_value());
  cpu_pressure(machine);
  machine.seconds(15);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);
}

// Scenario "Resume blocks re-suspension for 10 minutes", through the owner's choice.
TEST(GovernorLadder, OwnerResumeResumesOnlyWhatTheGovernorSuspended) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  memory_pressure(machine);
  machine.seconds(45);
  // Someone else suspends it once more: the owner's Resume undoes only the governor's suspend.
  ASSERT_TRUE(machine.os().suspend(os::ProcessKey{.pid = bigjob.pid, .created = bigjob.created}));
  EXPECT_EQ(machine.governor().choose(bigjob, governor::OwnerChoice::resume), 1U);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE actor = 'owner' AND outcome = "
                          "'resume' AND reason = 'resume'"),
            1);
}

// Scenario "Kill when opted in".
TEST(GovernorLadder, KillWhenEnabledForTheClass) {
  Machine machine;
  machine.policy().ladder.kill = {rules::ProcessClass::normal};
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  memory_pressure(machine);
  machine.seconds(55);
  EXPECT_FALSE(machine.process(bigjob).alive);
  EXPECT_EQ(machine.process(bigjob).exit_code, kill_exit_code);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE outcome = 'kill' AND reason = "
                          "'commit' AND metrics LIKE 'cpu=95.0%'"),
            1);
}

bool has_problem(Machine& machine, std::string_view text) {
  return std::ranges::any_of(machine.governor().problems(), [&](const std::string& problem) {
    return problem.find(text) != std::string::npos;
  });
}

// Audit risk 4: a metric goes missing, so the ladder gets no snapshot. After
// Governor::stall_limit its steps 1-3 are rolled back and the owner is told; the suspend waits
// for the owner, as when pressure clears.
TEST(GovernorLadder, MissingMetricRollsBackStepsAfterTheStallLimitButKeepsSuspends) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  const ProcessKey other =
      machine.spawn(400, R"(C:\Tools\other.exe)", std::nullopt, true, 10.0, 1_GiB);
  memory_pressure(machine);
  machine.seconds(45);  // bigjob up to its suspend, then other's first step
  ASSERT_EQ(machine.process(bigjob).suspend_count, 1U);
  ASSERT_EQ(machine.process(other).priority, os::PriorityClass::below_normal);
  ASSERT_EQ(jobs_of(machine.store(), JobRole::cap_job).size(), 1U);

  machine.metrics().erase(std::string(telemetry::metric::memory_commit));
  machine.seconds(static_cast<int>(Governor::stall_limit.count()) - 1);
  EXPECT_EQ(machine.process(other).priority, os::PriorityClass::below_normal);
  EXPECT_FALSE(has_problem(machine, "telemetry stalled"));
  machine.second();
  EXPECT_EQ(machine.process(other).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(other).memory_priority, os::MemoryPriority::normal);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  EXPECT_TRUE(jobs_of(machine.store(), JobRole::cap_job).empty());
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'roll_back' AND "
                          "reason = 'telemetry_stalled'"),
            2);
  EXPECT_TRUE(has_problem(machine, "memory.commit"));
  const nlohmann::json status = machine.governor().status();
  ASSERT_EQ(status.at("notifications").size(), 2U);
  EXPECT_EQ(status.at("notifications").at(0).at("step"), "suspend");

  // Once the metric is back the problem goes, and the ladder judges pressure afresh.
  machine.metrics()[std::string(telemetry::metric::memory_commit)] = 140.0 * gib;
  machine.second();
  EXPECT_FALSE(has_problem(machine, "telemetry stalled"));
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
}

// Audit risk 4: the process table stops advancing.
TEST(GovernorLadder, ProcessTableThatStopsAdvancingRollsBackSteps) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  cpu_pressure(machine);
  machine.seconds(25);
  ASSERT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::on);
  machine.freeze_view();
  machine.seconds(static_cast<int>(Governor::stall_limit.count()));
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  EXPECT_TRUE(has_problem(machine, "stopped advancing"));
  machine.thaw_view();
  machine.seconds(16);  // pressure judged afresh: 15 s above 90 % again, then step 1
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);
  EXPECT_FALSE(has_problem(machine, "telemetry stalled"));
}

// Audit risk 6: the root of a suspended tree exits while its child stays suspended. The
// notification stays, and its Resume reaches the child.
TEST(GovernorLadder, SuspendedTreeKeepsItsResumeWhenItsRootExits) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  const ProcessKey worker = machine.spawn(301, R"(C:\Tools\worker.exe)", bigjob, true, 30.0, 1_GiB);
  memory_pressure(machine);
  machine.seconds(45);
  ASSERT_EQ(machine.process(worker).suspend_count, 1U);

  machine.exit(bigjob);
  machine.seconds(2);
  const nlohmann::json status = machine.governor().status();
  ASSERT_EQ(status.at("notifications").size(), 1U);
  EXPECT_EQ(status.at("notifications").at(0).at("root").at("pid"), 300);
  EXPECT_EQ(status.at("notifications").at(0).at("step"), "suspend");

  EXPECT_EQ(machine.governor().choose(bigjob, governor::OwnerChoice::resume), 1U);
  EXPECT_EQ(machine.process(worker).suspend_count, 0U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_TRUE(machine.governor().status().at("notifications").empty());
}

// Once nothing of the tree carries a step, its notification goes with its root.
TEST(GovernorLadder, NotificationGoesWhenNoProcessOfItsTreeIsLeft) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  const ProcessKey worker = machine.spawn(301, R"(C:\Tools\worker.exe)", bigjob, true, 30.0, 1_GiB);
  memory_pressure(machine);
  machine.seconds(45);
  machine.exit(bigjob);
  machine.second();
  ASSERT_EQ(machine.governor().status().at("notifications").size(), 1U);
  machine.exit(worker);
  machine.seconds(2);
  EXPECT_TRUE(machine.governor().status().at("notifications").empty());
}

// Audit risk 6: more than ipc::max_notifications notifications; the oldest are dropped, but never
// the one of a suspended tree.
TEST(GovernorLadder, NotificationOfASuspendedTreeIsNeverTrimmed) {
  Machine machine;
  machine.policy().ladder.suspend = {rules::ProcessClass::build};
  machine.policy().ladder.cooldown = std::chrono::seconds{1};
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path, std::nullopt, true, 50.0, 8_GiB);
  for (std::uint32_t tool = 0; tool < 40; ++tool) {
    machine.spawn(500 + tool, std::format(R"(C:\Tools\tool{}.exe)", tool), std::nullopt, true, 1.0,
                  Bytes{(100U - tool) << 20U});
  }
  memory_pressure(machine);
  machine.seconds(200);
  ASSERT_EQ(machine.process(cargo).suspend_count, 1U);
  const nlohmann::json notifications = machine.governor().status().at("notifications");
  EXPECT_EQ(notifications.size(), ipc::max_notifications);
  EXPECT_TRUE(std::ranges::any_of(notifications, [](const nlohmann::json& notification) {
    return notification.at("root").at("pid") == 100 && notification.at("step") == "suspend";
  }));
}

// An owner's choice for a tree the ladder never acted on.
TEST(GovernorLadder, AllowOnceOnATreeWithoutStepsExemptsIt) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  machine.second();
  EXPECT_EQ(machine.governor().choose(bigjob, governor::OwnerChoice::allow_once), 0U);
  cpu_pressure(machine);
  machine.seconds(30);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);
}

// Audit risk 7: Windows refuses the owner's Resume. The process stays suspended and counted, its
// notification keeps Resume, the owner is told, and the governor tries again.
TEST(GovernorLadder, RefusedResumeStaysSuspendedIsReportedAndRetried) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  memory_pressure(machine);
  machine.seconds(45);
  ASSERT_EQ(machine.process(bigjob).suspend_count, 1U);

  machine.os().fail_next("resume", Error{.code = Errc::access_denied, .message = "denied"});
  EXPECT_EQ(machine.governor().choose(bigjob, governor::OwnerChoice::resume), 1U);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
  EXPECT_EQ(machine.count("SELECT SUM(count) FROM governor_suspend"), 1);
  nlohmann::json status = machine.governor().status();
  ASSERT_EQ(status.at("notifications").size(), 1U);
  EXPECT_EQ(status.at("notifications").at(0).at("step"), "suspend");
  EXPECT_EQ(status.at("trees").at(0).at("step"), "suspend");
  EXPECT_TRUE(has_problem(machine, "stay suspended"));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'resume' AND "
                          "actor = 'owner' AND detail LIKE '%1 failed (pid 300: denied)%'"),
            1);

  machine.seconds(static_cast<int>(Governor::resume_retry_interval.count()));
  EXPECT_EQ(machine.process(bigjob).suspend_count, 0U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  status = machine.governor().status();
  EXPECT_TRUE(status.at("notifications").empty());
  EXPECT_FALSE(has_problem(machine, "stay suspended"));
}

// After Governor::resume_retries refused retries the process waits for the owner's Resume.
TEST(GovernorLadder, RefusedResumeWaitsForTheOwnerAfterTheLastRetry) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  memory_pressure(machine);
  machine.seconds(45);
  for (int attempt = 0; attempt <= Governor::resume_retries; ++attempt) {
    machine.os().fail_next("resume", Error{.code = Errc::access_denied, .message = "denied"});
  }
  static_cast<void>(machine.governor().choose(bigjob, governor::OwnerChoice::resume));
  machine.seconds(static_cast<int>(Governor::resume_retry_interval.count()) *
                  (Governor::resume_retries + 2));
  EXPECT_EQ(machine.process(bigjob).suspend_count, 1U);
  EXPECT_TRUE(has_problem(machine, "stay suspended"));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'resume' AND "
                          "detail LIKE '%failed%'"),
            Governor::resume_retries + 1);

  EXPECT_EQ(machine.governor().choose(bigjob, governor::OwnerChoice::resume), 1U);
  EXPECT_EQ(machine.process(bigjob).suspend_count, 0U);
  EXPECT_FALSE(has_problem(machine, "stay suspended"));
}

// Audit risk 14: the owner's pause outlives a crash and a clean restart of the service.
TEST(GovernorLadder, PauseSurvivesARestartUntilItEnds) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  const std::optional<WallTime> until = machine.governor().pause(std::chrono::minutes{60});
  ASSERT_TRUE(until.has_value());
  const std::int64_t until_seconds = store::to_unix_seconds(until.value_or(WallTime{}));

  machine.crash();
  machine.start();
  EXPECT_EQ(machine.governor().status().at("paused_until"), until_seconds);
  cpu_pressure(machine);
  machine.seconds(30);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::normal);

  machine.governor().stop();
  machine.start();
  EXPECT_EQ(machine.governor().status().at("paused_until"), until_seconds);

  machine.clock().advance(std::chrono::minutes{60});
  machine.seconds(16);
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);
  EXPECT_TRUE(machine.governor().status().at("paused_until").is_null());
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_pause"), 0);
}

TEST(GovernorLadder, PauseThatEndedWhileTheServiceWasDownIsForgotten) {
  Machine machine;
  machine.start();
  ASSERT_TRUE(machine.governor().pause(std::chrono::minutes{5}).has_value());
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_pause"), 1);
  machine.crash();
  machine.clock().advance(std::chrono::minutes{10});
  machine.start();
  EXPECT_TRUE(machine.governor().status().at("paused_until").is_null());
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_pause"), 0);
  EXPECT_FALSE(machine.governor().pause(std::chrono::minutes{0}).has_value());
}

}  // namespace
}  // namespace myclean::governing
