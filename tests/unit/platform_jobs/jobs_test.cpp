// Win32JobControl against real job objects, with only the workload helper as members. Every job
// is created by the test, named Global\MyClean.Job.test.<nonce>, and released at the end.

#include "core/os/jobs.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "core/model/result.hpp"
#include "core/model/units.hpp"
#include "core/os/process_control.hpp"
#include "jobs_test_support.hpp"
#include "platform/jobs/win32_jobs.hpp"
#include "workload_process.hpp"

namespace myclean::platform::testing {
namespace {

using namespace std::chrono_literals;
using namespace myclean::literals;

constexpr std::uint32_t commit_limit_exit = 3;  // workload: ERROR_COMMITMENT_LIMIT

bool has_member(const os::IJob& job, std::uint32_t pid) {
  const Result<std::vector<std::uint32_t>> members = job.members();
  return members && std::ranges::find(*members, pid) != members->end();
}

// Polls `job` until active_process_zero or `timeout`.
std::vector<os::JobEvent> drain_until_empty(os::IJob& job, std::chrono::milliseconds timeout) {
  std::vector<os::JobEvent> events;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    Result<std::vector<os::JobEvent>> batch = job.poll(256);
    EXPECT_TRUE(batch) << batch.error().message;
    if (!batch) {
      break;
    }
    events.insert(events.end(), batch->begin(), batch->end());
    if (std::ranges::any_of(*batch, [](const os::JobEvent& event) {
          return event.kind == os::JobEventKind::active_process_zero;
        })) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  return events;
}

const os::JobEvent* find_event(const std::vector<os::JobEvent>& events, os::JobEventKind kind,
                               std::uint32_t pid) {
  const auto found = std::ranges::find_if(
      events, [&](const os::JobEvent& event) { return event.kind == kind && event.pid == pid; });
  return found == events.end() ? nullptr : &*found;
}

TEST_F(JobsTest, CreatesANamedJobWithAPort) {
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  EXPECT_EQ(job->ref().job_class, "test");
  EXPECT_TRUE(os::validate(job->ref()));
  EXPECT_TRUE(job->ref().object_name().starts_with("Global\\MyClean.Job.test."));
  EXPECT_EQ(job->permanent(), control().creates_permanent());
  EXPECT_TRUE(job->has_port());

  const Result<os::JobLimits> limits = job->limits();
  ASSERT_TRUE(limits) << limits.error().message;
  EXPECT_EQ(*limits, os::JobLimits{});
}

TEST_F(JobsTest, NoncesDiffer) {
  const std::unique_ptr<os::IJob> first = create();
  const std::unique_ptr<os::IJob> second = create();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first->ref().nonce, second->ref().nonce);
}

TEST_F(JobsTest, OpenChecksTheOwner) {
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);

  Result<std::unique_ptr<os::IJob>> opened = control().open(job->ref());
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ((*opened)->ref(), job->ref());
  EXPECT_FALSE((*opened)->has_port()) << "the creator's port stays the job's only port";
  EXPECT_EQ((*opened)->permanent(), job->permanent());

  // A job owned by anyone but the expected owner is untrusted.
  Win32JobControl expects_system;  // LocalSystem, as in the service
  const Result<std::unique_ptr<os::IJob>> as_system = expects_system.open(job->ref());
  ASSERT_FALSE(as_system);
  EXPECT_EQ(as_system.error().code, Errc::refused) << as_system.error().message;

  Win32JobControl expects_administrators{"S-1-5-32-544"};
  const Result<std::unique_ptr<os::IJob>> as_admins = expects_administrators.open(job->ref());
  ASSERT_FALSE(as_admins);
  EXPECT_EQ(as_admins.error().code, Errc::refused);
}

TEST_F(JobsTest, OpenOfAMissingJobIsNotFound) {
  const Result<std::unique_ptr<os::IJob>> opened =
      control().open(os::JobRef{.job_class = "test", .nonce = "0123456789abcdef0123456789abcdef"});
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code, Errc::not_found) << opened.error().message;

  const Result<std::unique_ptr<os::IJob>> malformed =
      control().open(os::JobRef{.job_class = "Test\\..", .nonce = "x"});
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().code, Errc::invalid_argument);
}

TEST_F(JobsTest, ReleaseRemovesTheNameAndEndsTheHandle) {
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  const os::JobRef ref = job->ref();
  ASSERT_TRUE(job->release());

  const Result<os::JobLimits> limits = job->limits();
  ASSERT_FALSE(limits);
  EXPECT_EQ(limits.error().code, Errc::invalid_state);
  EXPECT_FALSE(job->release());

  const Result<std::unique_ptr<os::IJob>> opened = control().open(ref);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code, Errc::not_found);
}

TEST_F(JobsTest, OrdinaryJobNameEndsWithItsLastHandle) {
  if (control().creates_permanent()) {
    GTEST_SKIP() << "this process holds SeCreatePermanentPrivilege, so its jobs are permanent";
  }
  std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  const os::JobRef ref = job->ref();
  job.reset();

  const Result<std::unique_ptr<os::IJob>> opened = control().open(ref);
  ASSERT_FALSE(opened);
  EXPECT_EQ(opened.error().code, Errc::not_found);
}

TEST_F(JobsTest, PermanentJobOutlivesItsHandles) {
  if (!control().creates_permanent()) {
    GTEST_SKIP() << "needs SeCreatePermanentPrivilege (run as LocalSystem); non-elevated jobs "
                    "are ordinary named jobs";
  }
  std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->permanent());
  const os::JobRef ref = job->ref();
  ASSERT_TRUE(job->set_cpu_cap(75.0));
  job.reset();  // as if the service crashed

  Result<std::unique_ptr<os::IJob>> reopened = control().open(ref);
  ASSERT_TRUE(reopened) << reopened.error().message;
  EXPECT_TRUE((*reopened)->permanent());
  const Result<os::JobLimits> limits = (*reopened)->limits();
  ASSERT_TRUE(limits);
  EXPECT_EQ(limits->cpu_cap_percent, 75.0);
  ASSERT_TRUE((*reopened)->release());

  const Result<std::unique_ptr<os::IJob>> gone = control().open(ref);
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().code, Errc::not_found);
}

TEST_F(JobsTest, LimitsAreSetReadAndLifted) {
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->set_cpu_cap(75.0));
  ASSERT_TRUE(job->set_commit_cap(1_GiB));
  ASSERT_TRUE(job->set_commit_notification(800_MiB));
  Result<os::JobLimits> limits = job->limits();
  ASSERT_TRUE(limits) << limits.error().message;
  EXPECT_EQ(*limits,
            (os::JobLimits{
                .cpu_cap_percent = 75.0, .commit_cap = 1_GiB, .commit_notification = 800_MiB}));

  ASSERT_TRUE(job->set_cpu_cap(40.0));  // game mode tightens a live job
  ASSERT_TRUE(job->clear_commit_notification());
  limits = job->limits();
  ASSERT_TRUE(limits);
  EXPECT_EQ(*limits, (os::JobLimits{.cpu_cap_percent = 40.0, .commit_cap = 1_GiB}));

  ASSERT_TRUE(job->lift_cpu_cap());
  ASSERT_TRUE(job->lift_commit_cap());
  limits = job->limits();
  ASSERT_TRUE(limits);
  EXPECT_EQ(*limits, os::JobLimits{});

  ASSERT_TRUE(job->set_cpu_cap(0.01));
  limits = job->limits();
  ASSERT_TRUE(limits);
  EXPECT_EQ(limits->cpu_cap_percent, 0.01);
}

TEST_F(JobsTest, LiftAllClearsEveryLimit) {
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->set_cpu_cap(50.0));
  ASSERT_TRUE(job->set_commit_cap(2_GiB));
  ASSERT_TRUE(job->set_commit_notification(1_GiB));

  ASSERT_TRUE(job->lift_all());
  const Result<os::JobLimits> limits = job->limits();
  ASSERT_TRUE(limits);
  EXPECT_EQ(*limits, os::JobLimits{});
}

TEST_F(JobsTest, InvalidLimitsAreRefused) {
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  for (const double percent : {0.0, -5.0, 100.5}) {
    const Result<void> set = job->set_cpu_cap(percent);
    ASSERT_FALSE(set) << percent;
    EXPECT_EQ(set.error().code, Errc::invalid_argument);
  }
  EXPECT_FALSE(job->set_commit_cap(Bytes{}));
  EXPECT_FALSE(job->set_commit_notification(Bytes{}));
  EXPECT_FALSE(control().create("Bad Class"));
}

TEST_F(JobsTest, AssignsAVerifiedProcess) {
  const Workload workload = Workload::start(L"exit 30000 0", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);

  Result<bool> contained = job->contains(workload.key());
  ASSERT_TRUE(contained) << contained.error().message;
  EXPECT_FALSE(*contained);

  ASSERT_TRUE(job->assign(workload.key()));
  ASSERT_TRUE(job->assign(workload.key())) << "a member is assigned again without error";
  contained = job->contains(workload.key());
  ASSERT_TRUE(contained);
  EXPECT_TRUE(*contained);
  EXPECT_TRUE(has_member(*job, workload.key().pid));
  const Result<bool> in_job = control().in_any_job(workload.key());
  ASSERT_TRUE(in_job);
  EXPECT_TRUE(*in_job);
}

TEST_F(JobsTest, RefusesAReusedPid) {
  const Workload workload = Workload::start(L"exit 30000 0", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  const os::ProcessKey older{.pid = workload.key().pid,
                             .created = workload.key().created - WallTime::duration{1}};

  const Result<void> assigned = job->assign(older);
  ASSERT_FALSE(assigned);
  EXPECT_EQ(assigned.error().code, Errc::not_found) << assigned.error().message;
  const Result<bool> contained = job->contains(older);
  ASSERT_FALSE(contained);
  EXPECT_EQ(contained.error().code, Errc::not_found);
  const Result<bool> in_job = control().in_any_job(older);
  ASSERT_FALSE(in_job);
  EXPECT_EQ(in_job.error().code, Errc::not_found);
  EXPECT_FALSE(has_member(*job, workload.key().pid));
}

TEST_F(JobsTest, RefusesAnExitedProcess) {
  const Workload workload = Workload::start(L"exit 0 0");
  ASSERT_TRUE(workload.started()) << workload.error();
  ASSERT_EQ(workload.wait(10s), 0U);
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);

  const Result<void> assigned = job->assign(workload.key());
  ASSERT_FALSE(assigned);
  EXPECT_EQ(assigned.error().code, Errc::not_found) << assigned.error().message;
}

// Spike X4 case A: a root already in a foreign (IDE) job gets a tree job nested under it.
TEST_F(JobsTest, NestsUnderAForeignJob) {
  const Workload workload = Workload::start(L"exit 30000 0", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> ide = create("ide");
  const std::unique_ptr<os::IJob> tree = create("build");
  ASSERT_NE(ide, nullptr);
  ASSERT_NE(tree, nullptr);
  ASSERT_TRUE(ide->assign(workload.key()));

  ASSERT_TRUE(os::assign_tree_root(workload.key(), nullptr, *tree));
  ASSERT_TRUE(tree->set_cpu_cap(40.0));
  EXPECT_TRUE(has_member(*ide, workload.key().pid));
  EXPECT_TRUE(has_member(*tree, workload.key().pid));
}

// Spike X4 §5.1 and case E: a job-free root goes to the class parent job, then to its tree job;
// a cap job can nest one level further.
TEST_F(JobsTest, NestsATreeUnderItsClassParentAndDeeper) {
  const Workload workload = Workload::start(L"exit 30000 0", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> parent = create("build");
  const std::unique_ptr<os::IJob> tree = create("build");
  const std::unique_ptr<os::IJob> cap = create("cap");
  ASSERT_NE(parent, nullptr);
  ASSERT_NE(tree, nullptr);
  ASSERT_NE(cap, nullptr);

  ASSERT_TRUE(os::assign_tree_root(workload.key(), parent.get(), *tree));
  ASSERT_TRUE(cap->assign(workload.key()));
  for (const os::IJob* job : {parent.get(), tree.get(), cap.get()}) {
    EXPECT_TRUE(has_member(*job, workload.key().pid)) << job->ref().object_name();
  }
}

// Spike X4 case C: one job cannot hold processes of two job hierarchies.
TEST_F(JobsTest, RefusesASecondHierarchy) {
  const Workload first = Workload::start(L"exit 30000 0", true);
  const Workload second = Workload::start(L"exit 30000 0", true);
  ASSERT_TRUE(first.started()) << first.error();
  ASSERT_TRUE(second.started()) << second.error();
  const std::unique_ptr<os::IJob> ide_a = create("ide");
  const std::unique_ptr<os::IJob> ide_b = create("ide");
  const std::unique_ptr<os::IJob> shared = create("build");
  ASSERT_NE(ide_a, nullptr);
  ASSERT_NE(ide_b, nullptr);
  ASSERT_NE(shared, nullptr);
  ASSERT_TRUE(ide_a->assign(first.key()));
  ASSERT_TRUE(ide_b->assign(second.key()));

  ASSERT_TRUE(shared->assign(first.key()));
  const Result<void> refused = shared->assign(second.key());
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, Errc::access_denied) << refused.error().message;
}

TEST_F(JobsTest, CpuCapHoldsOnAverage) {
  const DWORD cpus = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  const DWORD threads = std::max<DWORD>(2, cpus / 4);  // 25 % of the machine when not capped
  constexpr double cap = 10.0;
  const Workload workload = Workload::start(L"spin " + std::to_wstring(threads) + L" 8000", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->set_cpu_cap(cap));
  ASSERT_TRUE(job->assign(workload.key()));
  workload.resume();

  std::this_thread::sleep_for(1s);
  const Result<os::JobAccounting> before = job->accounting();
  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(2500ms);
  const Result<os::JobAccounting> after = job->accounting();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(before) << before.error().message;
  ASSERT_TRUE(after) << after.error().message;

  // Only an upper bound: other work on the machine may leave the job less than its cap.
  const double used = std::chrono::duration<double>(after->cpu_time - before->cpu_time).count();
  const double percent =
      100.0 * used / (std::chrono::duration<double>(elapsed).count() * static_cast<double>(cpus));
  std::cout << "job CPU over 2.5 s: " << percent << " % of " << cpus << " CPUs (cap " << cap
            << " %)\n";
  EXPECT_LE(percent, cap * 1.5);
  EXPECT_EQ(after->active_processes, 1U);
}

TEST_F(JobsTest, CommitCapFailsAllocationsInsideTheJobOnly) {
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->set_commit_cap(256_MiB));
  ASSERT_TRUE(job->set_commit_notification(128_MiB));

  // Holds what it got for 1 s: the notification is posted after the refused commit (spike X4
  // §3.2), and only while the job is still above the limit.
  const Workload inside = Workload::start(L"commit 16 512 1000", true);
  ASSERT_TRUE(inside.started()) << inside.error();
  ASSERT_TRUE(job->assign(inside.key()));
  inside.resume();
  EXPECT_EQ(inside.wait(30s), commit_limit_exit);

  const Workload outside = Workload::start(L"commit 16 512 0");
  ASSERT_TRUE(outside.started()) << outside.error();
  EXPECT_EQ(outside.wait(30s), 0U);

  const std::vector<os::JobEvent> events = drain_until_empty(*job, 10s);
  EXPECT_NE(find_event(events, os::JobEventKind::job_memory_limit, inside.key().pid), nullptr);
  const os::JobEvent* notification = find_event(events, os::JobEventKind::notification_limit, 0);
  ASSERT_NE(notification, nullptr);
  EXPECT_GE(notification->count, 1U);
  ASSERT_TRUE(notification->violation.has_value());
  EXPECT_EQ(notification->violation.value_or(os::LimitViolation{}).notification_limit, 128_MiB);
  EXPECT_EQ(std::ranges::count_if(events,
                                  [](const os::JobEvent& event) {
                                    return event.kind == os::JobEventKind::notification_limit;
                                  }),
            1)
      << "one poll folds every notification message into one event";

  ASSERT_TRUE(job->clear_commit_notification());
  const Result<os::JobLimits> limits = job->limits();
  ASSERT_TRUE(limits);
  EXPECT_EQ(*limits, (os::JobLimits{.commit_cap = 256_MiB}));
}

TEST_F(JobsTest, AccountingReportsTheJobCommit) {
  const Workload workload = Workload::start(L"commit 16 128 10000", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->assign(workload.key()));
  workload.resume();

  Result<os::JobAccounting> accounting = job->accounting();
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (accounting && accounting->committed < 128_MiB &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
    accounting = job->accounting();
  }
  ASSERT_TRUE(accounting) << accounting.error().message;
  EXPECT_GE(accounting->committed, 128_MiB);
  EXPECT_GE(accounting->peak_committed, accounting->committed);
  EXPECT_EQ(accounting->active_processes, 1U);
  EXPECT_EQ(accounting->total_processes, 1U);

  const Result<os::LimitViolation> violation = job->limit_violation();
  ASSERT_TRUE(violation);
  EXPECT_FALSE(violation->commit_exceeded);
  EXPECT_EQ(violation->notification_limit, Bytes{});
}

TEST_F(JobsTest, PortReportsChildStartAndExit) {
  const Workload root = Workload::start(L"child 1500 7", true);
  ASSERT_TRUE(root.started()) << root.error();
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->assign(root.key()));
  root.resume();

  const std::vector<os::JobEvent> events = drain_until_empty(*job, 20s);
  const os::JobEvent* root_started =
      find_event(events, os::JobEventKind::new_process, root.key().pid);
  ASSERT_NE(root_started, nullptr);
  EXPECT_EQ(root_started->created, root.key().created);

  const auto child_started = std::ranges::find_if(events, [&](const os::JobEvent& event) {
    return event.kind == os::JobEventKind::new_process && event.pid != root.key().pid;
  });
  ASSERT_NE(child_started, events.end());
  EXPECT_TRUE(child_started->created.has_value());
  const os::JobEvent* child_exited =
      find_event(events, os::JobEventKind::exit_process, child_started->pid);
  ASSERT_NE(child_exited, nullptr);
  EXPECT_EQ(child_exited->exit_code, 7U);
  EXPECT_EQ(child_exited->created, child_started->created);
  EXPECT_FALSE(child_exited->commit_failure);

  const os::JobEvent* root_exited =
      find_event(events, os::JobEventKind::exit_process, root.key().pid);
  ASSERT_NE(root_exited, nullptr);
  EXPECT_EQ(root_exited->exit_code, 0U);
  EXPECT_FALSE(events.empty());
  EXPECT_EQ(events.back().kind, os::JobEventKind::active_process_zero);
  EXPECT_FALSE(has_member(*job, root.key().pid));
}

TEST_F(JobsTest, PortClassifiesACommitFailureExit) {
  const Workload workload = Workload::start(L"exit 300 0xC0000017", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->assign(workload.key()));
  workload.resume();

  const std::vector<os::JobEvent> events = drain_until_empty(*job, 20s);
  const os::JobEvent* exited =
      find_event(events, os::JobEventKind::exit_process, workload.key().pid);
  ASSERT_NE(exited, nullptr);
  EXPECT_EQ(exited->exit_code, 0xC0000017U);
  EXPECT_TRUE(exited->commit_failure);
}

TEST_F(JobsTest, PollIsBounded) {
  const Workload workload = Workload::start(L"child 200 0", true);
  ASSERT_TRUE(workload.started()) << workload.error();
  const std::unique_ptr<os::IJob> job = create();
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(job->assign(workload.key()));
  workload.resume();
  ASSERT_EQ(workload.wait(20s), 0U);

  // new + exit for root and child, then active-process-zero: 5 messages, drained one by one.
  std::size_t drained = 0;
  for (int i = 0; i < 100; ++i) {
    const Result<std::vector<os::JobEvent>> batch = job->poll(1);
    ASSERT_TRUE(batch);
    ASSERT_LE(batch->size(), 1U);
    drained += batch->size();
    if (batch->empty()) {
      std::this_thread::sleep_for(20ms);
    } else if (batch->front().kind == os::JobEventKind::active_process_zero) {
      break;
    }
  }
  EXPECT_EQ(drained, 5U);
}

}  // namespace
}  // namespace myclean::platform::testing
