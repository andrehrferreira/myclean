#include "core/governor/recovery.hpp"

#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "core/governor/forest.hpp"
#include "core/governor/jobs.hpp"
#include "core/governor/policy.hpp"
#include "core/governor/reason.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"
#include "ladder_test_support.hpp"

namespace myclean::governor {
namespace {

using namespace myclean::literals;
using rules::ProcessClass;
using testing::key;

const PersistedJob build_job{.name = R"(Global\MyClean.Job.build.5f3a9c)",
                             .process_class = ProcessClass::build,
                             .role = JobRole::tree_job};

TEST(Recovery, RestartedServiceReAdoptsJobs) {
  JobPolicy policy;
  policy.build.cpu_percent = 60;  // the policy changed while the service was down
  const RecoveryInput input{
      .jobs = {build_job},
      .reopened = {
          {.name = build_job.name, .outcome = JobOpen::opened, .member_pids = {4100, 4200}}}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, policy);
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::adopt_job);
  EXPECT_EQ(commands[0].job_name, build_job.name);
  ASSERT_TRUE(commands[0].limits.has_value());
  const JobLimits limits = commands[0].limits.value_or(JobLimits{});
  EXPECT_EQ(limits.cpu_percent, 60);
  EXPECT_EQ(limits.commit, 96_GiB);
  EXPECT_EQ(commands[0].limits, class_job_limits(ProcessClass::build, policy, false));
}

TEST(Recovery, ReAdoptedBuildJobFollowsGameMode) {
  const RecoveryInput input{
      .jobs = {build_job},
      .reopened = {{.name = build_job.name, .outcome = JobOpen::opened, .member_pids = {4100}}},
      .game_mode = true};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].limits.value_or(JobLimits{}).cpu_percent, 40);
}

TEST(Recovery, JobWithoutMembersIsReleased) {
  const RecoveryInput input{
      .jobs = {build_job},
      .reopened = {{.name = build_job.name, .outcome = JobOpen::opened, .member_pids = {}}}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::release_job);
}

TEST(Recovery, CapJobIsReleasedAfterACrash) {
  const PersistedJob cap{.name = R"(Global\MyClean.Job.default.77aa01)",
                         .process_class = ProcessClass::normal,
                         .role = JobRole::cap_job};
  const RecoveryInput input{
      .jobs = {cap},
      .reopened = {{.name = cap.name, .outcome = JobOpen::opened, .member_pids = {5100}}}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::release_job);
}

TEST(Recovery, MissingJobIsForgotten) {
  const RecoveryInput input{.jobs = {build_job}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::forget_job);
}

TEST(Recovery, SquattedJobName) {
  const RecoveryInput input{
      .jobs = {build_job},
      .reopened = {{.name = build_job.name, .outcome = JobOpen::untrusted, .member_pids = {66}}}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::untrusted_job);
  EXPECT_EQ(commands[0].reason, Reason::untrusted_job);
  EXPECT_FALSE(commands[0].limits.has_value());
}

TEST(Recovery, ProcessSuspendedBySomeoneElse) {
  // A debugger's suspended process has no persisted record: nothing is resumed.
  const RecoveryInput input{.processes = {key(4100, 1), key(7000, 2)}};
  EXPECT_TRUE(plan_recovery(input, JobPolicy{}).empty());
}

TEST(Recovery, OrphanedSuspendResumed) {
  const RecoveryInput input{.suspends = {{.process = key(4100, 1), .count = 2}},
                            .processes = {key(4100, 1), key(7000, 2)}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::resume);
  EXPECT_EQ(commands[0].process, key(4100, 1));
  EXPECT_EQ(commands[0].times, 2);
  EXPECT_EQ(commands[0].reason, Reason::orphaned_suspend);
}

TEST(Recovery, ReusedPidIsNotResumed) {
  // PID 4100 now belongs to a process created later than the recorded one.
  const RecoveryInput input{.suspends = {{.process = key(4100, 1), .count = 1}},
                            .processes = {key(4100, 50)}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::forget_suspend);
}

TEST(Recovery, AccountedSuspendStays) {
  const RecoveryInput input{.suspends = {{.process = key(4100, 1), .count = 1}},
                            .processes = {key(4100, 1)},
                            .accounted = {key(4100, 1)}};
  EXPECT_TRUE(plan_recovery(input, JobPolicy{}).empty());
}

TEST(Recovery, NoOrphanLeftAfterKillAndRestart) {
  const PersistedJob cap{.name = R"(Global\MyClean.Job.default.0c0c0c)",
                         .process_class = ProcessClass::normal,
                         .role = JobRole::cap_job};
  const RecoveryInput input{
      .jobs = {build_job, cap},
      .reopened = {{.name = build_job.name, .outcome = JobOpen::opened, .member_pids = {4100}},
                   {.name = cap.name, .outcome = JobOpen::opened, .member_pids = {5100, 5200}}},
      .suspends = {{.process = key(5100, 3), .count = 1}, {.process = key(5200, 4), .count = 1}},
      .processes = {key(4100, 1), key(5100, 3), key(5200, 4)}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 4U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::adopt_job);    // limits the policy calls for
  EXPECT_EQ(commands[1].kind, RecoveryKind::release_job);  // the cap job: no ladder state left
  EXPECT_EQ(commands[2].kind, RecoveryKind::resume);
  EXPECT_EQ(commands[3].kind, RecoveryKind::resume);
}

TEST(Recovery, EmptyRecordIsForgotten) {
  const RecoveryInput input{.suspends = {{.process = key(4100, 1), .count = 0}},
                            .processes = {key(4100, 1)}};
  const std::vector<RecoveryCommand> commands = plan_recovery(input, JobPolicy{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands[0].kind, RecoveryKind::forget_suspend);
}

TEST(Recovery, Texts) {
  EXPECT_EQ(to_string(JobRole::class_job), "class");
  EXPECT_EQ(parse_job_role("cap"), JobRole::cap_job);
  EXPECT_FALSE(parse_job_role("other").has_value());
  EXPECT_EQ(to_string(RecoveryKind::untrusted_job), "untrusted_job");
}

}  // namespace
}  // namespace myclean::governor
