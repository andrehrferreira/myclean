// Crash recovery and job re-adoption (governor spec, "Crash Recovery and Job Re-Adoption"), over
// FakeJobs: a crash is the governor destroyed without stop().

#include "core/governor/recovery.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/policy.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"
#include "core/telemetry/metrics.hpp"
#include "governing_test_support.hpp"

namespace myclean::governing {
namespace {

using namespace myclean::literals;
using governor::JobRole;
using governor::ProcessKey;
using testing::jobs_of;
using testing::Machine;

constexpr const char* cargo_path = R"(C:\Users\owner\.cargo\bin\cargo.exe)";
constexpr const char* bigjob_path = R"(C:\Tools\bigjob.exe)";

os::ProcessKey os_key(const ProcessKey& key) {
  return {.pid = key.pid, .created = key.created};
}

// Scenario "Crash does not kill the build", "Job still reachable before the restart" and
// "Restarted service re-adopts jobs".
TEST(GovernorRecovery, RestartedServiceReAdoptsItsJobsWithTheCurrentPolicy) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const ProcessKey rustc = machine.spawn(200, R"(C:\rust\rustc.exe)", cargo);
  const JobRecord tree = jobs_of(machine.store(), JobRole::tree_job).at(0);
  machine.second();

  machine.crash();
  EXPECT_TRUE(machine.process(cargo).alive);
  EXPECT_TRUE(machine.process(rustc).alive);
  EXPECT_TRUE(machine.os().job_exists(tree.ref));
  EXPECT_EQ(machine.os().job_limits(tree.ref).cpu_cap_percent, 75.0);

  machine.policy().jobs.build.cpu_percent = 60;
  machine.start();
  EXPECT_EQ(jobs_of(machine.store(), JobRole::tree_job).at(0).ref, tree.ref);  // no new job
  EXPECT_EQ(machine.os().job_limits(tree.ref).cpu_cap_percent, 60.0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'job_adopted'"), 2);

  // Without a port its members are polled.
  machine.second();
  const nlohmann::json status = machine.governor().status();
  bool found = false;
  for (const nlohmann::json& job : status.at("jobs")) {
    if (job.at("name") == tree.ref.object_name()) {
      found = true;
      EXPECT_EQ(job.at("adopted"), true);
      EXPECT_EQ(job.at("members"), 2);
      EXPECT_EQ(job.at("cpu_cap_percent"), 60);
    }
  }
  EXPECT_TRUE(found);

  // A new child of the re-adopted tree gets no job of its own.
  machine.spawn(201, R"(C:\rust\rustc.exe)", cargo);
  EXPECT_EQ(jobs_of(machine.store(), JobRole::tree_job).size(), 1U);
}

// Scenario "Squatted job name".
TEST(GovernorRecovery, SquattedJobNameIsNotAdopted) {
  Machine machine;
  const os::JobRef squatted{.job_class = "build", .nonce = std::string(31, '0') + "a"};
  machine.os().plant_untrusted_job(squatted);
  ASSERT_TRUE(save_job(machine.store(), JobRecord{.ref = squatted,
                                                  .role = JobRole::tree_job,
                                                  .process_class = rules::ProcessClass::build,
                                                  .created = testing::noon}));
  machine.start();
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
  EXPECT_TRUE(machine.governor().status().at("jobs").empty());
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'untrusted_job' AND "
                          "reason = 'untrusted_job'"),
            1);
}

// Scenario "Process suspended by someone else" and "Orphaned suspend resumed": only recorded
// pairs are resumed, exactly as many times as recorded.
TEST(GovernorRecovery, OnlyRecordedSuspendsAreResumedAsManyTimesAsRecorded) {
  Machine machine;
  const ProcessKey debuggee = machine.spawn(400, R"(C:\Dev\debuggee.exe)", std::nullopt, false);
  ASSERT_TRUE(machine.os().suspend(os_key(debuggee)));
  const ProcessKey orphan = machine.spawn(401, bigjob_path, std::nullopt, false);
  for (int count = 0; count < 3; ++count) {
    ASSERT_TRUE(machine.os().suspend(os_key(orphan)));
  }
  // The governor's two suspends of `orphan`; the third is someone else's.
  ASSERT_TRUE(add_suspends(machine.store(), std::vector<ProcessKey>{orphan}));
  ASSERT_TRUE(add_suspends(machine.store(), std::vector<ProcessKey>{orphan}));
  // A record of a process that is gone.
  ASSERT_TRUE(add_suspends(
      machine.store(), std::vector<ProcessKey>{ProcessKey{.pid = 999, .created = testing::noon}}));

  machine.start();
  EXPECT_EQ(machine.process(debuggee).suspend_count, 1U);
  EXPECT_EQ(machine.process(orphan).suspend_count, 1U);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'resume' AND reason = "
                          "'orphaned_suspend' AND root_pid = 401"),
            1);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE outcome = 'resume' AND reason = "
                          "'orphaned_suspend'"),
            2);
}

// Scenario "No orphan left after kill and restart".
TEST(GovernorRecovery, NoCapOrSuspendSurvivesACrashAndRestart) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  machine.metrics()[std::string(telemetry::metric::cpu_utility)] = 95.0;
  // A memory shortage (commit at 140 GiB of 150 GiB): only then does the ladder suspend.
  machine.metrics()[std::string(telemetry::metric::memory_commit)] = 140.0 * 1024 * 1024 * 1024;
  machine.seconds(70);  // cargo's tree first (a build tree), then bigjob's
  ASSERT_EQ(machine.process(cargo).suspend_count, 1U);
  ASSERT_EQ(machine.process(bigjob).suspend_count, 1U);
  const os::JobRef cap = jobs_of(machine.store(), JobRole::cap_job).at(0).ref;
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;

  machine.crash();
  ASSERT_EQ(machine.os().job_limits(cap).cpu_cap_percent, 50.0);
  machine.start();
  EXPECT_EQ(machine.process(bigjob).suspend_count, 0U);
  EXPECT_EQ(machine.process(cargo).suspend_count, 0U);
  EXPECT_EQ(machine.os().job_limits(cap), os::JobLimits{});
  EXPECT_FALSE(machine.os().job_exists(cap));
  EXPECT_EQ(machine.os().job_limits(tree).cpu_cap_percent, 75.0);  // the policy calls for it
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_suspend"), 0);
  EXPECT_EQ(jobs_of(machine.store(), JobRole::cap_job).size(), 0U);
  EXPECT_TRUE(machine.process(cargo).alive);
}

// In observe mode a restarted service lifts what an act-mode run left instead of adopting it.
TEST(GovernorRecovery, ObserveModeReleasesJobsOfAnActRun) {
  Machine machine;
  machine.start();
  machine.spawn(100, cargo_path);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  machine.crash();
  machine.policy().mode = governor::GovernorMode::observe;
  machine.start();
  EXPECT_EQ(machine.os().job_limits(tree), os::JobLimits{});
  EXPECT_FALSE(machine.os().job_exists(tree));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
}

TEST(GovernorRecovery, JobWithoutMembersIsReleasedAndMissingJobForgotten) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  machine.crash();
  machine.exit(cargo);  // the governor is gone: FakeJobs only
  const os::JobRef missing{.job_class = "test", .nonce = std::string(31, '0') + "f"};
  ASSERT_TRUE(save_job(machine.store(), JobRecord{.ref = missing,
                                                  .role = JobRole::tree_job,
                                                  .process_class = rules::ProcessClass::test,
                                                  .created = testing::noon}));
  machine.start();
  EXPECT_FALSE(machine.os().job_exists(tree));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
}

// Audit risk 5 and NFR-REL-2 item 2: the priority, memory priority and EcoQoS the ladder saved are
// persisted, so a restarted service restores them after a crash.
TEST(GovernorRecovery, LadderStepsAreRestoredAfterACrash) {
  Machine machine;
  machine.start();
  const ProcessKey bigjob = machine.spawn(300, bigjob_path, std::nullopt, true, 60.0, 2_GiB);
  ASSERT_TRUE(machine.os().set_priority(os_key(bigjob), os::PriorityClass::above_normal));
  machine.metrics()[std::string(telemetry::metric::cpu_utility)] = 95.0;
  machine.seconds(25);  // priority, then EcoQoS
  ASSERT_EQ(machine.process(bigjob).priority, os::PriorityClass::below_normal);
  ASSERT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::on);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved WHERE pid = 300 AND priority = "
                          "'above_normal' AND memory_priority = 5 AND eco_qos = 'automatic'"),
            1);

  machine.crash();
  machine.start();
  EXPECT_EQ(machine.process(bigjob).priority, os::PriorityClass::above_normal);
  EXPECT_EQ(machine.process(bigjob).memory_priority, os::MemoryPriority::normal);
  EXPECT_EQ(machine.process(bigjob).eco_qos, os::EcoQos::automatic);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE outcome = 'roll_back' AND "
                          "reason = 'orphaned_state' AND detail LIKE '%pid 300%'"),
            1);
}

// A tree job adopted again keeps its root at BELOW_NORMAL, as the policy calls for; the stop
// then restores the priority it had before it was placed. Released in observe mode, it is restored
// at once.
TEST(GovernorRecovery, PlacementPriorityStaysWithAnAdoptedJobAndIsRestoredWhenItLeaves) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path, std::nullopt, false);
  ASSERT_TRUE(machine.os().set_priority(os_key(cargo), os::PriorityClass::above_normal));
  machine.start_event(cargo);
  ASSERT_EQ(machine.process(cargo).priority, os::PriorityClass::below_normal);

  machine.crash();
  machine.start();
  EXPECT_EQ(machine.process(cargo).priority, os::PriorityClass::below_normal);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved WHERE pid = 100 AND placed = "
                          "'above_normal'"),
            1);
  machine.governor().stop();
  EXPECT_EQ(machine.process(cargo).priority, os::PriorityClass::above_normal);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved"), 0);

  machine.start();
  machine.start_event(cargo);  // placed again (a new tree job)
  ASSERT_EQ(machine.process(cargo).priority, os::PriorityClass::below_normal);
  machine.crash();
  machine.policy().mode = governor::GovernorMode::observe;
  machine.start();
  EXPECT_EQ(machine.process(cargo).priority, os::PriorityClass::above_normal);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved"), 0);
}

TEST(GovernorRecovery, SavedStateOfAnExitedProcessIsForgotten) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved"), 1);
  machine.exit(cargo);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_saved"), 0);
}

}  // namespace
}  // namespace myclean::governing
