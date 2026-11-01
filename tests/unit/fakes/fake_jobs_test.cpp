#include "fakes/fake_jobs.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/jobs.hpp"
#include "core/os/process_control.hpp"

namespace myclean::fakes {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;

const WallTime t0 = std::chrono::sys_days{std::chrono::year{2026} / 11 / 1} + 9h;

std::unique_ptr<os::IJob> make(FakeJobs& jobs, const char* job_class = "build") {
  Result<std::unique_ptr<os::IJob>> job = jobs.create(job_class);
  if (!job) {
    throw std::logic_error(job.error().message);
  }
  return std::move(*job);
}

std::vector<os::JobEventKind> kinds(const std::vector<os::JobEvent>& events) {
  std::vector<os::JobEventKind> result;
  std::ranges::transform(events, std::back_inserter(result),
                         [](const os::JobEvent& event) { return event.kind; });
  return result;
}

TEST(FakeJobs, CreatesValidPermanentJobsWithAPort) {
  FakeJobs jobs;
  const std::unique_ptr<os::IJob> job = make(jobs);
  EXPECT_TRUE(os::validate(job->ref()));
  EXPECT_TRUE(job->permanent());
  EXPECT_TRUE(job->has_port());
  EXPECT_NE(make(jobs)->ref().nonce, job->ref().nonce);
  EXPECT_FALSE(jobs.create("Not Valid"));
}

TEST(FakeJobs, PermanentJobSurvivesItsHandlesUntilReleased) {
  FakeJobs jobs;
  std::unique_ptr<os::IJob> job = make(jobs);
  const os::JobRef ref = job->ref();
  ASSERT_TRUE(job->set_cpu_cap(75.0));
  job.reset();
  EXPECT_TRUE(jobs.job_exists(ref));

  Result<std::unique_ptr<os::IJob>> reopened = jobs.open(ref);
  ASSERT_TRUE(reopened);
  EXPECT_FALSE((*reopened)->has_port()) << "a job has one port for life";
  EXPECT_EQ((*reopened)->limits()->cpu_cap_percent, 75.0);
  ASSERT_TRUE((*reopened)->release());
  EXPECT_FALSE(jobs.job_exists(ref));
  EXPECT_EQ((*reopened)->limits().error().code, Errc::invalid_state);
  EXPECT_EQ(jobs.open(ref).error().code, Errc::not_found);
}

TEST(FakeJobs, OrdinaryJobNameEndsWithItsLastHandleButMembersKeepTheLimits) {
  FakeJobs jobs{false};
  const os::ProcessKey process = jobs.add_process(100, t0);
  std::unique_ptr<os::IJob> job = make(jobs);
  const os::JobRef ref = job->ref();
  ASSERT_TRUE(job->assign(process));
  ASSERT_TRUE(job->set_cpu_cap(40.0));
  job.reset();

  EXPECT_FALSE(jobs.job_exists(ref));
  EXPECT_EQ(jobs.open(ref).error().code, Errc::not_found);
  EXPECT_EQ(jobs.job_limits(ref).cpu_cap_percent, 40.0);
  EXPECT_TRUE(*jobs.in_any_job(process));
}

TEST(FakeJobs, UntrustedJobIsRefused) {
  FakeJobs jobs;
  const os::JobRef ref{.job_class = "build", .nonce = "0123456789abcdef0123456789abcdef"};
  jobs.plant_untrusted_job(ref);
  const Result<std::unique_ptr<os::IJob>> opened = jobs.open(ref);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code, Errc::refused);
}

TEST(FakeJobs, KeysAreVerified) {
  FakeJobs jobs;
  const os::ProcessKey old_process = jobs.add_process(200, t0);
  jobs.exit_process(old_process, 0);
  const os::ProcessKey reused = jobs.add_process(200, t0 + 1s);
  const std::unique_ptr<os::IJob> job = make(jobs);

  EXPECT_EQ(job->assign(old_process).error().code, Errc::not_found);
  EXPECT_EQ(jobs.suspend(old_process).error().code, Errc::not_found);
  EXPECT_TRUE(job->assign(reused));
  EXPECT_THROW((void)jobs.add_process(200, t0 + 2s), std::logic_error);
}

TEST(FakeJobs, PortReportsTreeMembership) {
  FakeJobs jobs;
  const os::ProcessKey root = jobs.add_process(10, t0);
  const std::unique_ptr<os::IJob> job = make(jobs);
  ASSERT_TRUE(job->assign(root));
  const os::ProcessKey child = jobs.start_child(root, 11, t0 + 1s);
  EXPECT_TRUE(*job->contains(child));
  jobs.exit_process(child, 0xC0000409U);
  jobs.exit_process(root, 0);

  const Result<std::vector<os::JobEvent>> events = job->poll(100);
  ASSERT_TRUE(events);
  EXPECT_EQ(kinds(*events),
            (std::vector{os::JobEventKind::new_process, os::JobEventKind::new_process,
                         os::JobEventKind::exit_process, os::JobEventKind::exit_process,
                         os::JobEventKind::active_process_zero}));
  EXPECT_EQ((*events)[1].pid, 11U);
  EXPECT_EQ((*events)[1].created, child.created);
  EXPECT_EQ((*events)[2].exit_code, 0xC0000409U);
  EXPECT_TRUE((*events)[2].commit_failure);
  EXPECT_FALSE((*events)[3].commit_failure);
  EXPECT_TRUE(job->members()->empty());
  EXPECT_EQ(job->accounting()->total_processes, 2U);
}

TEST(FakeJobs, PollIsBoundedAndPostedEventsArrive) {
  FakeJobs jobs;
  const std::unique_ptr<os::IJob> job = make(jobs);
  for (int i = 0; i < 3; ++i) {
    jobs.post(job->ref(), os::JobEvent{.kind = os::JobEventKind::notification_limit});
  }
  EXPECT_EQ(job->poll(2)->size(), 2U);
  EXPECT_EQ(job->poll(2)->size(), 1U);
  EXPECT_TRUE(job->poll(2)->empty());
}

TEST(FakeJobs, ReopenedJobHasNoPortAndListsItsMembers) {
  FakeJobs jobs;
  const os::ProcessKey process = jobs.add_process(30, t0);
  std::unique_ptr<os::IJob> job = make(jobs);
  const os::JobRef ref = job->ref();
  ASSERT_TRUE(job->assign(process));
  job.reset();  // the service crashed

  Result<std::unique_ptr<os::IJob>> reopened = jobs.open(ref);
  ASSERT_TRUE(reopened);
  EXPECT_FALSE((*reopened)->has_port());
  EXPECT_EQ((*reopened)->poll(10).error().code, Errc::invalid_state);
  EXPECT_EQ(*(*reopened)->members(), std::vector<std::uint32_t>{30});
}

TEST(FakeJobs, NestingFollowsTheFirstMember) {
  FakeJobs jobs;
  const os::ProcessKey in_a = jobs.add_process(1, t0);
  const os::ProcessKey in_b = jobs.add_process(2, t0);
  const os::ProcessKey free = jobs.add_process(3, t0);
  const std::unique_ptr<os::IJob> ide_a = make(jobs, "ide");
  const std::unique_ptr<os::IJob> ide_b = make(jobs, "ide");
  ASSERT_TRUE(ide_a->assign(in_a));
  ASSERT_TRUE(ide_b->assign(in_b));

  // Case C: the shared job joins A's hierarchy; B is refused; a job-free process is pulled in.
  const std::unique_ptr<os::IJob> shared = make(jobs);
  ASSERT_TRUE(shared->assign(in_a));
  EXPECT_EQ(shared->assign(in_b).error().code, Errc::access_denied);
  ASSERT_TRUE(shared->assign(free));
  EXPECT_TRUE(*ide_a->contains(free));

  // Case D: a job that took a job-free process first refuses one from a job.
  const os::ProcessKey other_free = jobs.add_process(4, t0);
  const std::unique_ptr<os::IJob> top = make(jobs);
  ASSERT_TRUE(top->assign(other_free));
  EXPECT_EQ(top->assign(in_b).error().code, Errc::access_denied);

  // Case E: chain A > shared > cap.
  const std::unique_ptr<os::IJob> cap = make(jobs, "cap");
  ASSERT_TRUE(cap->assign(in_a));
  EXPECT_TRUE(*cap->contains(in_a));
}

TEST(FakeJobs, AssignTreeRootNestsUnderTheClassParent) {
  FakeJobs jobs;
  const os::ProcessKey root = jobs.add_process(5, t0);
  const std::unique_ptr<os::IJob> parent = make(jobs);
  const std::unique_ptr<os::IJob> tree = make(jobs);
  ASSERT_TRUE(os::assign_tree_root(root, parent.get(), *tree));
  EXPECT_TRUE(*parent->contains(root));
  EXPECT_TRUE(*tree->contains(root));

  // A second job-free root of the same class shares the parent in its own tree job.
  const os::ProcessKey second = jobs.add_process(6, t0);
  const std::unique_ptr<os::IJob> second_tree = make(jobs);
  ASSERT_TRUE(os::assign_tree_root(second, parent.get(), *second_tree));
  EXPECT_EQ(parent->members()->size(), 2U);
  EXPECT_EQ(tree->members()->size(), 1U);
}

TEST(FakeJobs, LimitsAndUsage) {
  FakeJobs jobs;
  const std::unique_ptr<os::IJob> job = make(jobs);
  ASSERT_TRUE(job->set_cpu_cap(75.0));
  ASSERT_TRUE(job->set_commit_cap(96_GiB));
  ASSERT_TRUE(job->set_commit_notification(1_GiB));
  EXPECT_EQ(job->set_cpu_cap(0.0).error().code, Errc::invalid_argument);

  jobs.set_usage(job->ref(), 3s, 2_GiB);
  jobs.set_usage(job->ref(), 4s, 512_MiB);
  const Result<os::JobAccounting> accounting = job->accounting();
  ASSERT_TRUE(accounting);
  EXPECT_EQ(accounting->cpu_time, 4s);
  EXPECT_EQ(accounting->committed, 512_MiB);
  EXPECT_EQ(accounting->peak_committed, 2_GiB);
  EXPECT_FALSE(job->limit_violation()->commit_exceeded);
  jobs.set_usage(job->ref(), 5s, 1_GiB + 1_MiB);
  EXPECT_TRUE(job->limit_violation()->commit_exceeded);

  ASSERT_TRUE(job->lift_all());
  EXPECT_EQ(*job->limits(), os::JobLimits{});
}

TEST(FakeJobs, ProcessActionsReturnThePreviousState) {
  FakeJobs jobs;
  const os::ProcessKey process = jobs.add_process(7, t0);
  EXPECT_EQ(*jobs.set_priority(process, os::PriorityClass::below_normal),
            os::PriorityClass::normal);
  EXPECT_EQ(*jobs.set_memory_priority(process, os::MemoryPriority::low),
            os::MemoryPriority::normal);
  EXPECT_EQ(*jobs.set_eco_qos(process, os::EcoQos::on), os::EcoQos::automatic);
  EXPECT_EQ(*jobs.priority(process), os::PriorityClass::below_normal);

  ASSERT_TRUE(jobs.suspend(process));
  ASSERT_TRUE(jobs.suspend(process));
  ASSERT_TRUE(jobs.resume(process));
  EXPECT_EQ(jobs.process(process).suspend_count, 1U);
  ASSERT_TRUE(jobs.resume(process));
  ASSERT_TRUE(jobs.resume(process));
  EXPECT_EQ(jobs.process(process).suspend_count, 0U);

  ASSERT_TRUE(jobs.terminate(process, 9));
  EXPECT_FALSE(jobs.process(process).alive);
  EXPECT_EQ(jobs.process(process).exit_code, 9U);
  EXPECT_EQ(jobs.terminate(process, 9).error().code, Errc::not_found);
}

TEST(FakeJobs, InjectedFailuresAreReturnedOnce) {
  FakeJobs jobs;
  const os::ProcessKey process = jobs.add_process(8, t0);
  const std::unique_ptr<os::IJob> job = make(jobs);
  jobs.fail_next("assign", Error{.code = Errc::access_denied, .message = "elevated target"});
  EXPECT_EQ(job->assign(process).error().code, Errc::access_denied);
  EXPECT_TRUE(job->assign(process));

  jobs.fail_next("create", Error{.code = Errc::io_error, .message = "no memory"});
  EXPECT_FALSE(jobs.create("build"));
  EXPECT_TRUE(jobs.create("build"));
}

}  // namespace
}  // namespace myclean::fakes
