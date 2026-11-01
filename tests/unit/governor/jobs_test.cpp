#include "core/governor/jobs.hpp"

#include <optional>

#include <gtest/gtest.h>

#include "core/governor/policy.hpp"
#include "core/model/units.hpp"
#include "core/rules/schema.hpp"

namespace myclean::governor {
namespace {

using namespace myclean::literals;
using rules::ProcessClass;
using Action = CommitNotificationLimit::Action;

// The limits of a class that has jobs; a failure (and empty limits) for any other.
JobLimits limits_of(ProcessClass process_class, const JobPolicy& policy, bool game_mode) {
  const std::optional<JobLimits> limits = class_job_limits(process_class, policy, game_mode);
  EXPECT_TRUE(limits.has_value()) << rules::to_string(process_class);
  return limits.value_or(JobLimits{});
}

// ---- Job placement --------------------------------------------------------------------------

TEST(JobPlacement, BuildTreeCappedFromLaunch) {
  // `cargo build` from a terminal: in no job, so the class job first, then its own tree job.
  EXPECT_EQ(place_job(ProcessClass::build, false, JobMembership::none),
            JobPlacement::class_job_then_tree_job);
  EXPECT_EQ(place_job(ProcessClass::test, false, JobMembership::none),
            JobPlacement::class_job_then_tree_job);
}

TEST(JobPlacement, ProcessAlreadyInAnIdeJob) {
  EXPECT_EQ(place_job(ProcessClass::build, false, JobMembership::foreign), JobPlacement::tree_job);
}

TEST(JobPlacement, TreesFromDifferentJobHierarchies) {
  // Each root gets a tree job of its own; the IDE's never shares a job with the terminal's.
  EXPECT_EQ(place_job(ProcessClass::build, false, JobMembership::none),
            JobPlacement::class_job_then_tree_job);
  EXPECT_EQ(place_job(ProcessClass::build, false, JobMembership::foreign), JobPlacement::tree_job);
}

TEST(JobPlacement, ChildOfACappedTree) {
  // rustc started by cargo inherits cargo's tree job.
  EXPECT_EQ(place_job(ProcessClass::build, true, JobMembership::myclean), JobPlacement::none);
  EXPECT_EQ(place_job(ProcessClass::build, true, JobMembership::none), JobPlacement::none);
  EXPECT_EQ(place_job(ProcessClass::test, false, JobMembership::myclean), JobPlacement::none);
}

TEST(JobPlacement, OnlyBuildAndTestGetJobs) {
  for (const ProcessClass process_class :
       {ProcessClass::system, ProcessClass::game, ProcessClass::allow, ProcessClass::normal}) {
    EXPECT_EQ(place_job(process_class, false, JobMembership::none), JobPlacement::none);
  }
  EXPECT_EQ(to_string(JobPlacement::tree_job), "tree_job");
}

// ---- Job limits -----------------------------------------------------------------------------

TEST(JobLimits, DefaultCapsPerClass) {
  const JobPolicy policy;
  const JobLimits build = limits_of(ProcessClass::build, policy, false);
  EXPECT_EQ(build.cpu_percent, 75);
  EXPECT_EQ(build.commit, 96_GiB);
  const JobLimits test = limits_of(ProcessClass::test, policy, false);
  EXPECT_EQ(test.cpu_percent, 75);
  EXPECT_EQ(test.commit, 64_GiB);
  EXPECT_FALSE(class_job_limits(ProcessClass::normal, policy, false).has_value());
}

TEST(JobLimits, EarlyWarningAt80Percent) {
  const JobLimits build = limits_of(ProcessClass::build, JobPolicy{}, false);
  EXPECT_EQ(build.notification, Bytes{(96ULL << 30U) * 4 / 5});  // 76.8 GiB
}

TEST(JobLimits, GameModeLowersOnlyTheBuildCpuCap) {
  const JobPolicy policy;
  EXPECT_EQ(limits_of(ProcessClass::build, policy, true).cpu_percent, 40);
  EXPECT_EQ(limits_of(ProcessClass::test, policy, true).cpu_percent, 75);
  JobPolicy strict;
  strict.build.cpu_percent = 30;  // already below the game-mode cap: game mode never loosens it
  EXPECT_EQ(limits_of(ProcessClass::build, strict, true).cpu_percent, 30);
}

TEST(JobLimits, CapJobHeadroom) {
  const LadderPolicy policy;
  const JobLimits large = cap_job_limits(8_GiB, policy);
  EXPECT_EQ(large.cpu_percent, 50);
  EXPECT_EQ(large.commit, 10_GiB);
  EXPECT_FALSE(large.notification.has_value());
  EXPECT_EQ(cap_job_limits(Bytes{}, policy).commit, 64_MiB);
}

// ---- Commit notification limit --------------------------------------------------------------

TEST(CommitNotificationLimit, EarlyWarningAt80Percent) {
  CommitNotificationLimit limit(70);
  const Bytes cap = 96_GiB;
  EXPECT_EQ(limit.on_limit_message(), Action::record_and_clear);
  EXPECT_FALSE(limit.armed());
  // The flood of messages already queued records nothing more.
  for (int message = 0; message < 1000; ++message) {
    EXPECT_EQ(limit.on_limit_message(), Action::none);
  }
  // Still at or above 70 % of the cap: stays cleared.
  EXPECT_EQ(limit.on_sample(80_GiB, cap), Action::none);
  EXPECT_EQ(limit.on_sample(Bytes{(96ULL << 30U) * 7 / 10}, cap), Action::none);
  // Below 70 %: set again, once.
  EXPECT_EQ(limit.on_sample(60_GiB, cap), Action::rearm);
  EXPECT_TRUE(limit.armed());
  EXPECT_EQ(limit.on_sample(10_GiB, cap), Action::none);
  // The next crossing is recorded again.
  EXPECT_EQ(limit.on_limit_message(), Action::record_and_clear);
}

TEST(CommitNotificationLimit, ArmedLimitIgnoresSamples) {
  CommitNotificationLimit limit(70);
  EXPECT_EQ(limit.on_sample(1_GiB, 96_GiB), Action::none);
  EXPECT_TRUE(limit.armed());
}

}  // namespace
}  // namespace myclean::governor
