// Proactive build and test jobs, membership and the commit notification limit (governor spec,
// "Proactive Build and Test Jobs", "Tree Membership Tracking"), over FakeJobs.

#include "core/os/jobs.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/governing/records.hpp"
#include "core/governor/forest.hpp"
#include "core/governor/recovery.hpp"
#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"
#include "governing_test_support.hpp"

namespace myclean::governing {
namespace {

using namespace myclean::literals;
using governor::JobRole;
using governor::ProcessKey;
using testing::jobs_of;
using testing::Machine;

constexpr const char* cargo_path = R"(C:\Users\owner\.cargo\bin\cargo.exe)";
constexpr const char* rustc_path = R"(C:\Users\owner\.rustup\toolchains\stable\bin\rustc.exe)";

// `percent` of `bytes`, as the governor computes the notification limit.
Bytes share(Bytes bytes, std::uint64_t percent) {
  return Bytes{(bytes.count() / 100U * percent) + (bytes.count() % 100U * percent / 100U)};
}

const nlohmann::json* job_json(const nlohmann::json& status, const std::string& name) {
  for (const nlohmann::json& job : status.at("jobs")) {
    if (job.at("name") == name) {
      return &job;
    }
  }
  return nullptr;
}

// Scenario "Build tree capped from launch" and "Child of a capped tree".
TEST(GovernorJobs, BuildTreeIsCappedFromLaunchAndChildrenJoinIt) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);

  const std::vector<JobRecord> trees = jobs_of(machine.store(), JobRole::tree_job);
  const std::vector<JobRecord> classes = jobs_of(machine.store(), JobRole::class_job);
  ASSERT_EQ(trees.size(), 1U);
  ASSERT_EQ(classes.size(), 1U);
  EXPECT_EQ(trees[0].root, cargo);
  EXPECT_EQ(trees[0].root_image, "cargo.exe");
  EXPECT_EQ(trees[0].ref.job_class, "build");
  // A root in no job goes to the class job first, so its tree job nests under it.
  EXPECT_EQ(machine.process(cargo).jobs,
            (std::vector<std::string>{classes[0].ref.object_name(), trees[0].ref.object_name()}));
  for (const JobRecord& job : {trees[0], classes[0]}) {
    const os::JobLimits& limits = machine.os().job_limits(job.ref);
    EXPECT_EQ(limits.cpu_cap_percent, 75.0);
    EXPECT_EQ(limits.commit_cap, 96_GiB);
    EXPECT_EQ(limits.commit_notification, share(96_GiB, 80));
  }
  EXPECT_EQ(machine.process(cargo).priority, os::PriorityClass::below_normal);

  // 32 rustc: no job of their own, members of cargo's tree job by inheritance.
  std::vector<ProcessKey> rustc;
  for (std::uint32_t pid = 200; pid < 232; ++pid) {
    rustc.push_back(machine.spawn(pid, rustc_path, cargo));
  }
  EXPECT_EQ(jobs_of(machine.store(), JobRole::tree_job).size(), 1U);
  EXPECT_EQ(jobs_of(machine.store(), JobRole::class_job).size(), 1U);
  for (const ProcessKey& child : rustc) {
    EXPECT_EQ(machine.process(child).jobs, machine.process(cargo).jobs);
  }

  // Membership from the completion port's messages.
  machine.second();
  const nlohmann::json status = machine.governor().status();
  const nlohmann::json* tree = job_json(status, trees[0].ref.object_name());
  ASSERT_NE(tree, nullptr);
  EXPECT_EQ(tree->at("members"), 33);
  EXPECT_EQ(tree->at("role"), "tree");
  EXPECT_EQ(tree->at("cpu_cap_percent"), 75);

  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job'"), 1);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE event = 'GovernorStep' AND "
                          "outcome = 'place_job' AND actor = 'governor' AND plan_id = 0"),
            1);
}

// Scenario "Process already in an IDE job" and "Trees from different job hierarchies".
TEST(GovernorJobs, TreeInAForeignJobNestsUnderItAndTerminalTreeGetsItsOwn) {
  Machine machine;
  machine.start();
  const ProcessKey devenv = machine.spawn(50, R"(C:\VS\Common7\IDE\devenv.exe)");
  Result<std::unique_ptr<os::IJob>> ide = machine.os().create("ide");
  ASSERT_TRUE(ide.has_value());
  ASSERT_TRUE((*ide)->assign(os::ProcessKey{.pid = devenv.pid, .created = devenv.created}));

  const ProcessKey msbuild = machine.spawn(60, R"(C:\VS\MSBuild\Current\Bin\MSBuild.exe)", devenv);
  const ProcessKey cargo = machine.spawn(100, cargo_path);

  const std::vector<JobRecord> trees = jobs_of(machine.store(), JobRole::tree_job);
  const std::vector<JobRecord> classes = jobs_of(machine.store(), JobRole::class_job);
  ASSERT_EQ(trees.size(), 2U);
  ASSERT_EQ(classes.size(), 1U);  // only the job-free cargo used the class job
  const JobRecord& msbuild_tree = trees[0].root == msbuild ? trees[0] : trees[1];
  const JobRecord& cargo_tree = trees[0].root == cargo ? trees[0] : trees[1];
  EXPECT_EQ(
      machine.process(msbuild).jobs,
      (std::vector<std::string>{(*ide)->ref().object_name(), msbuild_tree.ref.object_name()}));
  EXPECT_EQ(machine.process(cargo).jobs,
            (std::vector<std::string>{classes[0].ref.object_name(), cargo_tree.ref.object_name()}));
  EXPECT_EQ(machine.os().job_limits(msbuild_tree.ref).cpu_cap_percent, 75.0);
  EXPECT_EQ(machine.os().job_limits(msbuild_tree.ref).commit_cap, 96_GiB);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job' AND "
                          "detail LIKE '%not placed%'"),
            0);
}

TEST(GovernorJobs, TestClassGetsItsOwnCaps) {
  Machine machine;
  machine.start();
  machine.spawn(100, R"(C:\Python\Scripts\pytest.exe)");
  const std::vector<JobRecord> trees = jobs_of(machine.store(), JobRole::tree_job);
  ASSERT_EQ(trees.size(), 1U);
  EXPECT_EQ(trees[0].ref.job_class, "test");
  EXPECT_EQ(machine.os().job_limits(trees[0].ref).commit_cap, 64_GiB);
}

TEST(GovernorJobs, OtherClassesGetNoJob) {
  Machine machine;
  machine.start();
  machine.spawn(100, R"(C:\Windows\System32\notepad.exe)");
  machine.spawn(101, R"(C:\Windows\System32\svchost.exe)");
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
}

TEST(GovernorJobs, ForegroundBuildProcessIsLeftAlone) {
  Machine machine;
  machine.start();
  machine.governor().set_user_state(
      UserState{.state = games::NotificationState::accepts_notifications, .foreground_pid = 100});
  machine.spawn(100, cargo_path);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
}

// Scenario "Early warning at 80 %".
TEST(GovernorJobs, CommitNotificationIsRecordedOnceAndReArmedBelow70Percent) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  const auto notifications = [&] {
    return machine.count(
        "SELECT COUNT(*) FROM governor_event WHERE kind = 'notification_limit' AND detail LIKE "
        "'tree job%'");
  };

  // Above 76.8 GiB: the 1 s loop sees it in the job's accounting even without a port message.
  machine.os().set_usage(tree, std::chrono::seconds{1}, 77_GiB);
  machine.second();
  EXPECT_EQ(notifications(), 1);
  EXPECT_FALSE(machine.os().job_limits(tree).commit_notification.has_value());

  // A flood of messages while above: still one event.
  machine.os().post(tree,
                    os::JobEvent{.kind = os::JobEventKind::notification_limit, .count = 5000});
  machine.os().set_usage(tree, std::chrono::seconds{2}, 80_GiB);
  machine.seconds(3);
  EXPECT_EQ(notifications(), 1);
  EXPECT_FALSE(machine.os().job_limits(tree).commit_notification.has_value());

  // Between 70 % and 80 %: still cleared. Below 70 % (67.2 GiB): set again.
  machine.os().set_usage(tree, std::chrono::seconds{3}, 70_GiB);
  machine.second();
  EXPECT_FALSE(machine.os().job_limits(tree).commit_notification.has_value());
  machine.os().set_usage(tree, std::chrono::seconds{4}, 60_GiB);
  machine.second();
  EXPECT_EQ(machine.os().job_limits(tree).commit_notification, share(96_GiB, 80));

  machine.os().set_usage(tree, std::chrono::seconds{5}, 78_GiB);
  machine.second();
  EXPECT_EQ(notifications(), 2);
  static_cast<void>(cargo);
}

TEST(GovernorJobs, CommitCapRefusalAndCommitFailureExitAreRecorded) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const ProcessKey rustc = machine.spawn(200, rustc_path, cargo);
  const os::JobRef tree = jobs_of(machine.store(), JobRole::tree_job).at(0).ref;
  machine.second();
  for (int message = 0; message < 3; ++message) {
    machine.os().post(tree, os::JobEvent{.kind = os::JobEventKind::job_memory_limit, .pid = 200});
  }
  machine.exit(rustc, 0xC0000409U);
  machine.second();
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'job_memory_limit' "
                          "AND reason = 'commit' AND detail LIKE 'tree job%pid 200%'"),
            1);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = "
                          "'commit_failure_exit' AND detail LIKE 'tree job%0xC0000409%'"),
            1);
}

// "Tree Membership Tracking": a tree job whose root exited while members remain stays; it is
// released when its last member exits.
TEST(GovernorJobs, TreeJobIsReleasedWhenItsLastMemberExits) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path);
  const ProcessKey server = machine.spawn(200, R"(C:\VS\mspdbsrv.exe)", cargo);
  const JobRecord tree = jobs_of(machine.store(), JobRole::tree_job).at(0);
  const JobRecord parent = jobs_of(machine.store(), JobRole::class_job).at(0);
  machine.second();

  machine.exit(cargo);
  machine.second();
  EXPECT_TRUE(machine.os().job_exists(tree.ref));
  EXPECT_EQ(jobs_of(machine.store(), JobRole::tree_job).size(), 1U);

  machine.exit(server);
  machine.second();
  EXPECT_FALSE(machine.os().job_exists(tree.ref));
  EXPECT_FALSE(machine.os().job_exists(parent.ref));
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'job_released'"), 2);
}

// Audit risk 3: the start event of a build root is handled after the root started children.
// Those in the process table join its tree job when it is placed; a protected one does not.
TEST(GovernorJobs, ChildrenStartedBeforeTheRootWasPlacedJoinItsTreeJob) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path, std::nullopt, false);
  const ProcessKey rustc = machine.spawn(200, rustc_path, cargo, false);
  const ProcessKey linker = machine.spawn(201, R"(C:\VS\link.exe)", rustc, false);
  const ProcessKey host = machine.spawn(202, R"(C:\Windows\System32\svchost.exe)", cargo, false);
  const ProcessKey below_host = machine.spawn(203, R"(C:\Tools\helper.exe)", host, false);
  machine.second();

  machine.start_event(cargo);
  const std::vector<std::string> jobs = machine.process(cargo).jobs;
  ASSERT_EQ(jobs.size(), 2U);  // class job, tree job
  for (const ProcessKey& child : {rustc, linker}) {
    EXPECT_EQ(machine.process(child).jobs, jobs);
    EXPECT_EQ(machine.process(child).priority, os::PriorityClass::below_normal);
  }
  for (const ProcessKey& spared : {host, below_host}) {
    EXPECT_TRUE(machine.process(spared).jobs.empty());
    EXPECT_EQ(machine.process(spared).priority, os::PriorityClass::normal);
  }
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job' AND "
                          "root_pid = 100 AND detail LIKE '%2 processes it started earlier joined "
                          "it%'"),
            1);

  // Their own start events change nothing more: they are in the job already.
  machine.start_event(rustc);
  machine.start_event(linker);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job'"), 1);
  EXPECT_EQ(jobs_of(machine.store(), JobRole::tree_job).size(), 1U);
}

// A child too new for the process table joins when its own start event comes, and so do its
// children in turn.
TEST(GovernorJobs, ChildMissingFromTheProcessTableJoinsOnItsStartEvent) {
  Machine machine;
  machine.start();
  const ProcessKey cargo = machine.spawn(100, cargo_path, std::nullopt, false);
  const ProcessKey rustc = machine.spawn(200, rustc_path, cargo, false);
  const ProcessKey linker = machine.spawn(201, R"(C:\VS\link.exe)", rustc, false);
  machine.hide(rustc);
  machine.hide(linker);
  machine.start_event(cargo);
  EXPECT_TRUE(machine.process(rustc).jobs.empty());

  machine.start_event(rustc);
  machine.start_event(linker);
  for (const ProcessKey& child : {rustc, linker}) {
    EXPECT_EQ(machine.process(child).jobs, machine.process(cargo).jobs);
    EXPECT_EQ(machine.process(child).priority, os::PriorityClass::below_normal);
  }
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job' AND "
                          "detail LIKE 'joined tree job%started before its root was placed%'"),
            2);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM action_log WHERE outcome = 'place_job'"), 3);
  machine.show(rustc);
  machine.show(linker);
  machine.second();
  EXPECT_EQ(machine.governor().status().at("jobs").at(1).at("members"), 3);
}

TEST(GovernorJobs, FailedAssignmentLeavesNoJobBehind) {
  Machine machine;
  machine.start();
  machine.os().fail_next("assign", Error{.code = Errc::access_denied, .message = "denied"});
  machine.spawn(100, cargo_path);
  machine.second();
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_job"), 0);
  EXPECT_EQ(machine.count("SELECT COUNT(*) FROM governor_event WHERE kind = 'place_job' AND "
                          "detail LIKE 'not placed%denied%'"),
            1);
}

}  // namespace
}  // namespace myclean::governing
